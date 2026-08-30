/*=============================================================================
 * Device-only suite: cube app performance profiling.
 *
 * Measures frame budget breakdown for the rotating cube app over 10 seconds:
 * - Total frame time (wall clock)
 * - Simulation/logic time (spinning, small3dlib scene setup)
 * - Rasterization time (small3dlib pixel callback execution)
 * - Present time (QSPI DMA transfer)
 * - Reports: min, max, average, median, p95
 *
 * Runs under DEVICE_BUILD only - needs real panel, DMA, and framebuffer.
 *===========================================================================*/
#include "suites.h"   /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "unity.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "gfx/gfx.h"
#include "app.h"
#include "ui/ui.h"

/* app_cube.c's own toggle - forced true in the fixture below so a stray
 * BOOT-menu setting left over from manual testing can never silently skew
 * a perf run. */
extern bool partial_updates;

/* app_cube.c's three per-frame phases plus its enter/exit, all exposed
 * specifically for this suite. Deliberately NOT S3L_newFrame()/
 * S3L_drawScene() or the cube/scene state directly: small3dlib.h defines
 * real, non-static functions once configured and included, so only the
 * translation unit that already includes it (app_cube.c) can touch them -
 * a second #include here would redefine those same symbols and fail to
 * link. Going through cube_update_rotation()/cube_clear_frame()/
 * cube_rasterize_frame() instead means this suite exercises the exact
 * code cube_frame() runs, not a hand-copy of it that could drift. */
extern void cube_enter(void);
extern void cube_exit(void);
extern void cube_update_rotation(uint32_t dt_ms);
extern void cube_clear_frame(void);
extern void cube_rasterize_frame(void);
extern void draw_fps(const input_t *input);

static const char *TAG = "cube_perf";

#define SAMPLE_SECONDS  10
#define SAMPLE_MS       (SAMPLE_SECONDS * 1000)

/* Frame timing breakdown. int32_t, not int64_t: these are one frame's worth
 * of microseconds, always well under a few hundred thousand, and halving
 * their size matters here - MAX_SAMPLES of these plus stat_scratch below
 * are static, and the diag image's free heap was already close enough to
 * POST's 40 KiB floor that the int64_t version of this file tripped it. */
typedef struct {
    int32_t frame_total_us;    /* wall clock per frame */
    int32_t logic_us;          /* cube logic + scene setup */
    int32_t rasterize_us;      /* small3dlib S3L_drawScene() */
    int32_t hud_us;            /* draw_fps() - zero when with_hud is false */
    int32_t present_us;        /* gfx_present() */
} frame_sample_t;

/* A genuine ring buffer, not a 10s-at-60fps-sized capture: run_perf_capture()
 * runs for the full SAMPLE_SECONDS regardless of how many frames that turns
 * out to be, wrapping sample_count % MAX_SAMPLES back to the start once the
 * ring fills, so stats are always taken over the most recent MAX_SAMPLES
 * frames rather than whichever frames happened to land first. That is a
 * feature, not just a memory saving: a fixed capture cap would silently
 * truncate the window's tail on a device rendering faster than expected,
 * biasing every stat toward the run's startup transient instead of its
 * settled frame rate. sample_count itself is never wrapped - it is the
 * total frame count used for the reported average fps - only the index
 * into samples[]/stat_scratch[] is.
 *
 * 128 is deliberately smaller than a 10s capture would ever need: it is
 * still enough for a meaningful P95 (128 * 5% = 6 samples in the tail), and
 * every sample here costs five int32_t fields, four of them duplicated a
 * second time in stat_scratch while compute_stats() sorts one field at a
 * time - see its own comment. If a future run ever wants closer to the
 * full window's true distribution instead of its recent tail, blending two
 * such rings - e.g. an exponential moving average of each ring's own
 * min/max/median as one drains into the other - would buy that back
 * without ever paying for the whole 10s of raw samples at once; nothing
 * here needs that precision yet. */
#define MAX_SAMPLES  128
static frame_sample_t samples[MAX_SAMPLES];
static int sample_count = 0;

static int cmp_i32(const void *a, const void *b)
{
    int32_t va = *(const int32_t *)a;
    int32_t vb = *(const int32_t *)b;
    return (va > vb) - (va < vb);
}

/* Median helper: sorts `arr` in place and returns its middle element -
 * compute_stats() below relies on it having already been sorted here to
 * read p95 out of the same array right after, rather than sorting again
 * itself. */
static int32_t median_of(int32_t *arr, int n)
{
    if (n == 0) return 0;
    qsort(arr, n, sizeof(int32_t), cmp_i32);
    return arr[n / 2];
}

typedef enum {
    FIELD_TOTAL,
    FIELD_LOGIC,
    FIELD_RASTERIZE,
    FIELD_HUD,
    FIELD_PRESENT,
} sample_field_t;

static int32_t field_of(const frame_sample_t *s, sample_field_t field)
{
    switch (field) {
        case FIELD_TOTAL:     return s->frame_total_us;
        case FIELD_LOGIC:     return s->logic_us;
        case FIELD_RASTERIZE: return s->rasterize_us;
        case FIELD_HUD:       return s->hud_us;
        case FIELD_PRESENT:   return s->present_us;
    }
    return 0;
}

typedef struct {
    int64_t min, max, avg, med, p95;
} phase_stats_t;

/* Scratch space for whichever field compute_stats() is sorting right now -
 * shared and reused across all five calls rather than one MAX_SAMPLES
 * array per field, which is what overflowed the main task's stack when it
 * was five stack-local arrays, and starved gfx's own allocations when
 * moved to five static ones instead. One reused buffer costs a fifth of
 * either. */
static int32_t stat_scratch[MAX_SAMPLES];

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
    s.med = median_of(stat_scratch, n);        /* sorts stat_scratch in place */
    s.p95 = stat_scratch[(n * 95) / 100];       /* stat_scratch is now sorted */
    return s;
}

static void cube_perf_fixture(void)
{
    /* draw_fps() needs ctx->text_width/text_height, which only ui_init()
     * sets - normally done once by the shell's own startup, which the
     * selftest runs before (see main.c's app_main(): selftest_run() runs
     * ahead of ui_launcher_init()). Without this, draw_fps() trips
     * microui's own assertion the first time it tries to lay out text.
     * suite_ui.c's fixture does the same for the same reason. */
    ui_init();

    /* Use the app's own enter to set up cube, scene, etc. */
    cube_enter();

    /* Force partial updates on regardless of whatever the BOOT menu was
     * last left at - cube_clear_frame() reads this flag itself, so this is
     * the one place that has to guarantee it rather than trusting ambient
     * state. */
    partial_updates = true;
    gfx_set_interlace(false);

    sample_count = 0;
}

static void cube_perf_teardown(void)
{
    cube_exit();

    /* gfx_set_interlace() is gfx.c-global state, not app-scoped like
     * partial_clear (cube_exit() already turns that off) - left on here,
     * it would leak into every suite that runs after this one in the same
     * boot, since suites are registered and run alphabetically and
     * "cube_perf" sorts right before "display"/"gfx_*". That is exactly
     * what broke their own dirty-tracking budget assertions the first time
     * this suite ran on device: interlace's carried-over dirty bits made
     * an otherwise-unchanged frame look like it still had pixels to send. */
    gfx_set_interlace(false);
}

/* Nothing pressed, no touch - draw_fps() feeds this straight into
 * ui_begin()/feed_input(), which dereference it unconditionally, so a real
 * (zeroed) input_t is required here, not NULL. */
static const input_t null_input = { 0 };

/* Runs the 10-second capture and writes /spiffs/cube_perf_<label>_<ts>.md.
 * `with_hud` toggles the one line real cube_frame() always pays for -
 * draw_fps() - timed as its own phase so a with/without run shows exactly
 * what the HUD text costs, rather than folding it silently into whichever
 * phase happened to run next. */
static void run_perf_capture(const char *label, bool with_hud)
{
    cube_perf_fixture();
    
    int64_t test_start = esp_timer_get_time();
    int64_t next_frame_due = test_start;

    /* Run at natural frame rate (no vTaskDelay) for the full SAMPLE_SECONDS -
     * no frame-count cap here, since samples[] is a ring buffer (see its own
     * comment) rather than a fixed capture, so there is nothing left that
     * needs one. */
    while (esp_timer_get_time() - test_start < SAMPLE_MS * 1000) {
        int64_t frame_start = esp_timer_get_time();
        int64_t dt_ms = (frame_start - next_frame_due) / 1000;
        if (dt_ms < 0) dt_ms = 1;
        if (dt_ms > 250) dt_ms = 250;
        next_frame_due += dt_ms * 1000;
        
        /* --- LOGIC PHASE --- */
        int64_t logic_start = esp_timer_get_time();
        cube_update_rotation((uint32_t)dt_ms);
        cube_clear_frame();
        int64_t logic_end = esp_timer_get_time();

        /* --- RASTERIZE PHASE --- */
        int64_t raster_start = esp_timer_get_time();
        cube_rasterize_frame();
        int64_t raster_end = esp_timer_get_time();

        /* --- HUD PHASE --- */
        /* Same position in the frame real cube_frame() calls it from - after
         * the cube's own dirty region is marked, before gfx_present() goes
         * out. Skipped entirely when with_hud is false, so hud_us reads as
         * ~0 rather than the cost of a no-op draw_fps() call. */
        int64_t hud_start = esp_timer_get_time();
        if (with_hud) {
            draw_fps(&null_input);
        }
        int64_t hud_end = esp_timer_get_time();

        /* --- PRESENT PHASE --- */
        int64_t present_start = esp_timer_get_time();
        gfx_present();
        int64_t present_end = esp_timer_get_time();

        int64_t frame_end = present_end;

        /* Store sample - wraps into the ring rather than growing forever;
         * sample_count itself keeps counting every frame, ring wrap or not,
         * since it also doubles as the true total for the average-fps math
         * below. */
        int idx = sample_count % MAX_SAMPLES;
        samples[idx].frame_total_us = (int32_t)(frame_end - frame_start);
        samples[idx].logic_us = (int32_t)(logic_end - logic_start);
        samples[idx].rasterize_us = (int32_t)(raster_end - raster_start);
        samples[idx].hud_us = with_hud ? (int32_t)(hud_end - hud_start) : 0;
        samples[idx].present_us = (int32_t)(present_end - present_start);
        sample_count++;
    }

    /* --- COMPUTE STATISTICS --- */
    if (sample_count == 0) {
        ESP_LOGE(TAG, "No frames captured!");
        cube_perf_teardown();
        TEST_FAIL();
        return;
    }

    /* The ring only ever holds this many valid entries even once
     * sample_count (the true total) has grown past it. */
    const int valid = (sample_count < MAX_SAMPLES) ? sample_count : MAX_SAMPLES;

    phase_stats_t total = compute_stats(FIELD_TOTAL, valid);
    phase_stats_t logic = compute_stats(FIELD_LOGIC, valid);
    phase_stats_t rast  = compute_stats(FIELD_RASTERIZE, valid);
    phase_stats_t hud   = compute_stats(FIELD_HUD, valid);
    phase_stats_t pres  = compute_stats(FIELD_PRESENT, valid);

    /* --- OUTPUT MARKDOWN REPORT --- */
    char report_path[128];
    snprintf(report_path, sizeof(report_path),
             "/spiffs/cube_perf_%s_%lld.md", label,
             (long long)(test_start / 1000000));

    FILE *f = fopen(report_path, "w");
    if (f) {
        fprintf(f, "# Cube App Performance Report (%s)\n\n", label);
        fprintf(f, "Captured: %lld frames over %d seconds\n\n", (long long)sample_count, SAMPLE_SECONDS);

        fprintf(f, "## Frame Total (wall clock)\n");
        fprintf(f, "| Stat | us | fps |\n");
        fprintf(f, "|---|---:|---:|\n");
        fprintf(f, "| Min | %lld | %.1f |\n", (long long)total.min, 1000000.0 / total.min);
        fprintf(f, "| Max | %lld | %.1f |\n", (long long)total.max, 1000000.0 / total.max);
        fprintf(f, "| Average | %lld | %.1f |\n", (long long)total.avg, 1000000.0 / total.avg);
        fprintf(f, "| Median | %lld | %.1f |\n", (long long)total.med, 1000000.0 / total.med);
        fprintf(f, "| P95 | %lld | %.1f |\n\n", (long long)total.p95, 1000000.0 / total.p95);

        fprintf(f, "## Breakdown\n");
        fprintf(f, "| Phase | Min | Max | Avg | Median | P95 |\n");
        fprintf(f, "|---|---:|---:|---:|---:|---:|\n");
        fprintf(f, "| Logic | %lld | %lld | %lld | %lld | %lld |\n",
                (long long)logic.min, (long long)logic.max, (long long)logic.avg,
                (long long)logic.med, (long long)logic.p95);
        fprintf(f, "| Rasterize | %lld | %lld | %lld | %lld | %lld |\n",
                (long long)rast.min, (long long)rast.max, (long long)rast.avg,
                (long long)rast.med, (long long)rast.p95);
        fprintf(f, "| HUD (draw_fps) | %lld | %lld | %lld | %lld | %lld |\n",
                (long long)hud.min, (long long)hud.max, (long long)hud.avg,
                (long long)hud.med, (long long)hud.p95);
        fprintf(f, "| Present | %lld | %lld | %lld | %lld | %lld |\n",
                (long long)pres.min, (long long)pres.max, (long long)pres.avg,
                (long long)pres.med, (long long)pres.p95);
        fprintf(f, "| **Total** | %lld | %lld | %lld | %lld | %lld |\n\n",
                (long long)total.min, (long long)total.max, (long long)total.avg,
                (long long)total.med, (long long)total.p95);

        fprintf(f, "## Frame Budget vs Target (60 fps = 16667 us)\n");
        fprintf(f, "| Phase | Budget %% (avg) |\n");
        fprintf(f, "|---|---:|\n");
        fprintf(f, "| Logic | %.1f%% |\n", (double)logic.avg / 16667.0 * 100.0);
        fprintf(f, "| Rasterize | %.1f%% |\n", (double)rast.avg / 16667.0 * 100.0);
        fprintf(f, "| HUD (draw_fps) | %.1f%% |\n", (double)hud.avg / 16667.0 * 100.0);
        fprintf(f, "| Present | %.1f%% |\n", (double)pres.avg / 16667.0 * 100.0);
        fprintf(f, "| **Total** | %.1f%% |\n\n", (double)total.avg / 16667.0 * 100.0);

        fclose(f);
        ESP_LOGI(TAG, "Report written to %s", report_path);
    } else {
        ESP_LOGW(TAG, "Could not open %s for writing (SPIFFS not mounted?)", report_path);
    }

    /* Also log to console for immediate visibility. */
    ESP_LOGI(TAG, "=== CUBE PERF %s (%d frames) ===", label, sample_count);
    ESP_LOGI(TAG, "Total:   avg=%lldus med=%lldus p95=%lldus  (%.1f/%.1f/%.1f fps)",
             (long long)total.avg, (long long)total.med, (long long)total.p95,
             1000000.0/total.avg, 1000000.0/total.med, 1000000.0/total.p95);
    ESP_LOGI(TAG, "Logic:   avg=%lldus med=%lldus (%.1f%%)",
             (long long)logic.avg, (long long)logic.med, (double)logic.avg/total.avg*100);
    ESP_LOGI(TAG, "Raster:  avg=%lldus med=%lldus (%.1f%%)",
             (long long)rast.avg, (long long)rast.med, (double)rast.avg/total.avg*100);
    ESP_LOGI(TAG, "HUD:     avg=%lldus med=%lldus (%.1f%%)",
             (long long)hud.avg, (long long)hud.med, (double)hud.avg/total.avg*100);
    ESP_LOGI(TAG, "Present: avg=%lldus med=%lldus (%.1f%%)",
             (long long)pres.avg, (long long)pres.med, (double)pres.avg/total.avg*100);

    cube_perf_teardown();
}

void test_cube_performance_over_10s(void)
{
    run_perf_capture("with_hud", true);
    TEST_PASS();
}

void test_cube_performance_over_10s_no_hud(void)
{
    run_perf_capture("no_hud", false);
    TEST_PASS();
}

/* Additional test: measure with interlace mode enabled. */
void test_cube_performance_interlaced(void)
{
    cube_perf_fixture();
    gfx_set_interlace(true);
    
    /* Quick 100-frame sample instead of 10s. */
    sample_count = 0;

    for (int i = 0; i < 100 && sample_count < MAX_SAMPLES; i++) {
        int64_t frame_start = esp_timer_get_time();

        cube_update_rotation(16);  /* ~60fps target */
        cube_clear_frame();
        cube_rasterize_frame();
        gfx_present();

        samples[sample_count].frame_total_us = (int32_t)(esp_timer_get_time() - frame_start);
        sample_count++;
    }
    
    int64_t total_sum = 0, total_min = INT64_MAX, total_max = 0;
    for (int i = 0; i < sample_count; i++) {
        int64_t t = samples[i].frame_total_us;
        total_sum += t;
        if (t < total_min) total_min = t;
        if (t > total_max) total_max = t;
    }
    int64_t total_avg = total_sum / sample_count;
    
    ESP_LOGI(TAG, "INTERLACED (100 frames): avg=%lldus (%.1f fps) min=%lld max=%lld",
             (long long)total_avg, 1000000.0/total_avg, (long long)total_min, (long long)total_max);
    
    cube_perf_teardown();
    TEST_PASS();
}

void run_cube_perf_suite(void)
{
    RUN_TEST(test_cube_performance_over_10s);
    RUN_TEST(test_cube_performance_over_10s_no_hud);
    RUN_TEST(test_cube_performance_interlaced);
}

#else  /* !DEVICE_BUILD */

void run_cube_perf_suite(void) { }

#endif  /* DEVICE_BUILD */

SUITE_REGISTER(run_cube_perf_suite);
