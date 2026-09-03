/*=============================================================================
 * Device-only suite: boot animation performance profiling.
 *
 * Unlike suite_cube_perf.c's steady-state spinning cube, boot_anim_draw_
 * frame() is a pure function of now_ms, and the whole point of this suite
 * is that its cost is NOT steady across the animation's own timeline: grid
 * rings arrive progressively, the space transform's scale grows across
 * keyframes, and the photograph's crossfade adds a full-framebuffer blend
 * on top of whatever else is drawing. A single flat-out capture the way
 * cube_perf runs one would average all of that away.
 *
 * So this FREEZES time instead of letting it run: boot_anim_draw_frame()
 * being a pure function of now_ms means calling it repeatedly at one fixed
 * timestamp is a legitimate, repeatable measurement of exactly what that
 * moment in the animation costs - not a hand-picked scene standing in for
 * it. A handful of checkpoints (see build_checkpoints() below), each timed
 * per-phase (clear/floor/axes/curve/zeros/image/title/present) the same
 * way cube_perf breaks down logic/rasterize/hud/present, with the same
 * min/max/avg/median/p95 report.
 *
 * Runs under DEVICE_BUILD only - needs real panel, DMA, and framebuffer.
 *===========================================================================*/
#include "suites.h"   /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <stdlib.h>

#include "unity.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "gfx/gfx.h"
#include "boot/boot_anim.h"
#include "boot/boot_anim_timeline.h"

/* boot_anim.c's own draw_* functions, exposed specifically for this suite -
 * see boot_anim.c's own comment above draw_floor() for why they are
 * non-static rather than declared in boot_anim.h. Calling these directly,
 * not a hand-copy of boot_anim_draw_frame()'s own sequencing, means this
 * suite exercises the exact code a real frame runs. */
extern void boot_anim_clear_frame(void);
extern void draw_floor(uint32_t now_ms, uint8_t ink,
                       const boot_anim_view_t *view);
extern void draw_axes(uint32_t now_ms, uint8_t ink,
                      const boot_anim_view_t *view);
extern int32_t draw_curve(uint32_t now_ms, uint8_t ink,
                          const boot_anim_view_t *view);
extern void draw_zeros(int32_t pen_t_q8, uint8_t ink,
                       const boot_anim_view_t *view);
extern void draw_image(uint8_t ink, uint8_t reveal);
extern void draw_title(uint32_t now_ms, uint8_t ink);

static const char *TAG = "boot_anim_perf";

/* Each checkpoint freezes now_ms and repeats the frame this many times -
 * not a time-bounded ring buffer the way cube_perf's open-ended natural-
 * rate capture needs one: the workload here is deterministic per
 * checkpoint, so a fixed count is both simpler and exactly as much data as
 * cube_perf's own 128-sample ring ever guarantees anyway. Small enough
 * that six checkpoints' worth still finishes in well under a minute. */
#define SAMPLES_PER_CHECKPOINT 60

typedef struct {
    int32_t frame_total_us;
    int32_t clear_us;
    int32_t floor_us;
    int32_t axes_us;
    int32_t curve_us;
    int32_t zeros_us;
    int32_t image_us;
    int32_t title_us;
    int32_t present_us;
} frame_sample_t;

/* Heap-allocated per checkpoint, freed right after its own report - the
 * same reason suite_cube_perf.c's samples/stat_scratch are heap, not
 * static: a full selftest run walks every suite in one boot, and a static
 * array here would be permanent .bss cost paid by every suite after this
 * one in the same run, not just while this one is executing. */
static frame_sample_t *samples = NULL;
static int32_t *stat_scratch = NULL;

static int cmp_i32(const void *a, const void *b)
{
    int32_t va = *(const int32_t *)a;
    int32_t vb = *(const int32_t *)b;
    return (va > vb) - (va < vb);
}

typedef enum {
    FIELD_TOTAL, FIELD_CLEAR, FIELD_FLOOR, FIELD_AXES, FIELD_CURVE,
    FIELD_ZEROS, FIELD_IMAGE, FIELD_TITLE, FIELD_PRESENT,
} sample_field_t;

static int32_t field_of(const frame_sample_t *s, sample_field_t field)
{
    switch (field) {
        case FIELD_TOTAL:   return s->frame_total_us;
        case FIELD_CLEAR:   return s->clear_us;
        case FIELD_FLOOR:   return s->floor_us;
        case FIELD_AXES:    return s->axes_us;
        case FIELD_CURVE:   return s->curve_us;
        case FIELD_ZEROS:   return s->zeros_us;
        case FIELD_IMAGE:   return s->image_us;
        case FIELD_TITLE:   return s->title_us;
        case FIELD_PRESENT: return s->present_us;
    }
    return 0;
}

typedef struct {
    int64_t min, max, avg, med, p95;
} phase_stats_t;

static phase_stats_t compute_stats(sample_field_t field, int n)
{
    phase_stats_t s = { .min = INT64_MAX, .max = 0, .avg = 0, .med = 0, .p95 = 0 };
    int64_t sum = 0;

    for (int i = 0; i < n; i++) {
        int32_t v = field_of(&samples[i], field);
        stat_scratch[i] = v;
        if (v < s.min) s.min = v;
        if (v > s.max) s.max = v;
        sum += v;
    }

    s.avg = sum / n;
    qsort(stat_scratch, n, sizeof(int32_t), cmp_i32);
    s.med = stat_scratch[n / 2];
    s.p95 = stat_scratch[(n * 95) / 100];
    return s;
}

typedef struct {
    const char *label;
    uint32_t now_ms;
} checkpoint_t;

static uint32_t clamp_below(uint32_t ms, uint32_t exclusive_max)
{
    if (exclusive_max == 0) return 0;
    return ms < exclusive_max ? ms : exclusive_max - 1;
}

/* Six moments along the animation's own timeline, all derived from the
 * generated constants rather than hand-guessed literals - boot_anim_
 * timeline.json is the author's own actively-tuned file (see its own
 * comment history), so a checkpoint written as a raw ms number would
 * silently stop meaning what its label says the moment the timeline is
 * retuned. clamp_below() only matters if a future retune makes one
 * checkpoint's natural formula land past where it is meant to stay inside
 * (grid settling past IMAGE_START_MS, or any of them past BOOT_ANIM_MS
 * itself) - normal today, but not guaranteed to stay that way. */
static void build_checkpoints(checkpoint_t out[6])
{
    const uint32_t grid_settled = BOOT_ANIM_GRID_START_MS +
        (uint32_t)BOOT_ANIM_GRID_RINGS * BOOT_ANIM_GRID_RING_MS +
        BOOT_ANIM_GRID_FADE_MS;
    const uint32_t image_start = (uint32_t)BOOT_ANIM_IMAGE_START_MS;

    out[0] = (checkpoint_t){"curve_climbing", clamp_below(
        BOOT_ANIM_PEN_START_MS + BOOT_ANIM_PEN_MS / 2, BOOT_ANIM_MS)};
    out[1] = (checkpoint_t){"title_flying_in", clamp_below(
        BOOT_ANIM_TITLE_START_MS + 200, BOOT_ANIM_MS)};
    out[2] = (checkpoint_t){"grid_settled_pre_image", clamp_below(
        grid_settled, image_start < BOOT_ANIM_MS ? image_start : BOOT_ANIM_MS)};
    out[3] = (checkpoint_t){"crossfade_start", clamp_below(
        image_start + 50, BOOT_ANIM_MS)};
    out[4] = (checkpoint_t){"crossfade_mid", clamp_below(
        image_start + BOOT_ANIM_IMAGE_FADE_MS / 2, BOOT_ANIM_MS)};
    out[5] = (checkpoint_t){"near_end", clamp_below(
        BOOT_ANIM_MS > 100 ? BOOT_ANIM_MS - 100 : 0, BOOT_ANIM_MS)};
}

static void run_checkpoint(const checkpoint_t *cp)
{
    samples = malloc(sizeof(frame_sample_t) * SAMPLES_PER_CHECKPOINT);
    stat_scratch = malloc(sizeof(int32_t) * SAMPLES_PER_CHECKPOINT);
    if (samples == NULL || stat_scratch == NULL) {
        free(samples);
        free(stat_scratch);
        samples = NULL;
        stat_scratch = NULL;
        TEST_FAIL_MESSAGE("need samples and stat_scratch buffers for the "
                           "boot_anim perf capture, and at least one of the "
                           "two failed to allocate");
    }

    const uint32_t now_ms = cp->now_ms;
    const uint8_t ink    = boot_anim_ink(now_ms);
    const uint8_t reveal = boot_anim_image_reveal(now_ms);
    const uint8_t scene  = boot_anim_scene_reach(now_ms);
    const boot_anim_view_t view = boot_anim_view(GFX_WIDTH, GFX_HEIGHT, now_ms);
    const bool draw_scene = scene > 0;
    const bool draw_title_now = now_ms >= BOOT_ANIM_TITLE_START_MS;

    for (int i = 0; i < SAMPLES_PER_CHECKPOINT; i++) {
        int64_t t0, t1;
        frame_sample_t *s = &samples[i];
        const int64_t frame_start = esp_timer_get_time();

        t0 = esp_timer_get_time();
        boot_anim_clear_frame();
        t1 = esp_timer_get_time();
        s->clear_us = (int32_t)(t1 - t0);

        if (draw_scene) {
            t0 = esp_timer_get_time();
            draw_floor(now_ms, ink, &view);
            t1 = esp_timer_get_time();
            s->floor_us = (int32_t)(t1 - t0);

            t0 = esp_timer_get_time();
            draw_axes(now_ms, ink, &view);
            t1 = esp_timer_get_time();
            s->axes_us = (int32_t)(t1 - t0);

            t0 = esp_timer_get_time();
            const int32_t reached = draw_curve(now_ms, ink, &view);
            t1 = esp_timer_get_time();
            s->curve_us = (int32_t)(t1 - t0);

            t0 = esp_timer_get_time();
            draw_zeros(reached, ink, &view);
            t1 = esp_timer_get_time();
            s->zeros_us = (int32_t)(t1 - t0);
        } else {
            s->floor_us = 0;
            s->axes_us = 0;
            s->curve_us = 0;
            s->zeros_us = 0;
        }

        t0 = esp_timer_get_time();
        draw_image(ink, reveal);
        t1 = esp_timer_get_time();
        s->image_us = (int32_t)(t1 - t0);

        if (draw_title_now) {
            t0 = esp_timer_get_time();
            draw_title(now_ms, ink);
            t1 = esp_timer_get_time();
            s->title_us = (int32_t)(t1 - t0);
        } else {
            s->title_us = 0;
        }

        t0 = esp_timer_get_time();
        gfx_present();
        t1 = esp_timer_get_time();
        s->present_us = (int32_t)(t1 - t0);

        s->frame_total_us = (int32_t)(t1 - frame_start);
    }

    phase_stats_t total   = compute_stats(FIELD_TOTAL, SAMPLES_PER_CHECKPOINT);
    phase_stats_t clear_s = compute_stats(FIELD_CLEAR, SAMPLES_PER_CHECKPOINT);
    phase_stats_t floor_s = compute_stats(FIELD_FLOOR, SAMPLES_PER_CHECKPOINT);
    phase_stats_t axes_s  = compute_stats(FIELD_AXES, SAMPLES_PER_CHECKPOINT);
    phase_stats_t curve_s = compute_stats(FIELD_CURVE, SAMPLES_PER_CHECKPOINT);
    phase_stats_t zeros_s = compute_stats(FIELD_ZEROS, SAMPLES_PER_CHECKPOINT);
    phase_stats_t image_s = compute_stats(FIELD_IMAGE, SAMPLES_PER_CHECKPOINT);
    phase_stats_t title_s = compute_stats(FIELD_TITLE, SAMPLES_PER_CHECKPOINT);
    phase_stats_t pres_s  = compute_stats(FIELD_PRESENT, SAMPLES_PER_CHECKPOINT);

    ESP_LOGI(TAG, "=== BOOT_ANIM PERF %s (now_ms=%u, %d samples) ===",
             cp->label, (unsigned)now_ms, SAMPLES_PER_CHECKPOINT);
    ESP_LOGI(TAG, "Total:   min=%lldus max=%lldus avg=%lldus med=%lldus p95=%lldus (%.1f/%.1f/%.1f fps)",
             (long long)total.min, (long long)total.max, (long long)total.avg,
             (long long)total.med, (long long)total.p95,
             1000000.0/total.avg, 1000000.0/total.med, 1000000.0/total.p95);
    ESP_LOGI(TAG, "Clear:   avg=%lldus (%.1f%%)", (long long)clear_s.avg,
             (double)clear_s.avg/total.avg*100);
    ESP_LOGI(TAG, "Floor:   avg=%lldus (%.1f%%)", (long long)floor_s.avg,
             (double)floor_s.avg/total.avg*100);
    ESP_LOGI(TAG, "Axes:    avg=%lldus (%.1f%%)", (long long)axes_s.avg,
             (double)axes_s.avg/total.avg*100);
    ESP_LOGI(TAG, "Curve:   avg=%lldus (%.1f%%)", (long long)curve_s.avg,
             (double)curve_s.avg/total.avg*100);
    ESP_LOGI(TAG, "Zeros:   avg=%lldus (%.1f%%)", (long long)zeros_s.avg,
             (double)zeros_s.avg/total.avg*100);
    ESP_LOGI(TAG, "Image:   min=%lldus max=%lldus avg=%lldus med=%lldus p95=%lldus (%.1f%%)",
             (long long)image_s.min, (long long)image_s.max, (long long)image_s.avg,
             (long long)image_s.med, (long long)image_s.p95,
             (double)image_s.avg/total.avg*100);
    ESP_LOGI(TAG, "Title:   avg=%lldus (%.1f%%)", (long long)title_s.avg,
             (double)title_s.avg/total.avg*100);
    ESP_LOGI(TAG, "Present: min=%lldus max=%lldus avg=%lldus med=%lldus p95=%lldus (%.1f%%)",
             (long long)pres_s.min, (long long)pres_s.max, (long long)pres_s.avg,
             (long long)pres_s.med, (long long)pres_s.p95,
             (double)pres_s.avg/total.avg*100);

    free(samples);
    free(stat_scratch);
    samples = NULL;
    stat_scratch = NULL;
}

void test_boot_anim_performance_by_checkpoint(void)
{
    gfx_clear_clip();
    gfx_set_partial_clear(false);
    gfx_invalidate();

    checkpoint_t checkpoints[6];
    build_checkpoints(checkpoints);

    for (int i = 0; i < 6; i++) {
        run_checkpoint(&checkpoints[i]);
    }

    TEST_PASS();
}

void run_boot_anim_perf_suite(void)
{
    RUN_TEST(test_boot_anim_performance_by_checkpoint);
}

#else /* !DEVICE_BUILD */

void run_boot_anim_perf_suite(void) {}

#endif

SUITE_REGISTER(run_boot_anim_perf_suite);
