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

/* The cube app's internal state - these are static in app_cube.c so we need
 * to declare them as extern to access from the test. In a real test you'd
 * typically put these in a shared header, but for this performance profiling
 * tool we just declare them here. */
extern S3L_Model3D cube;
extern S3L_Scene   scene;
extern uint32_t    elapsed_ms;
extern int         frame_x0, frame_y0, frame_x1, frame_y1;
extern bool        partial_updates;
extern bool        menu_open;
extern uint32_t    fps_frame_count;
extern uint32_t    fps_window_elapsed_ms;
extern double      fps_value;
extern uint32_t    last_layout_generation;

/* Functions from app_cube.c */
extern void cube_enter(void);
extern void cube_exit(void);
extern void cube_frame(uint32_t dt_ms, const input_t *input);

static const char *TAG = "cube_perf";

#define SAMPLE_SECONDS  10
#define SAMPLE_MS       (SAMPLE_SECONDS * 1000)

/* Frame timing breakdown. */
typedef struct {
    int64_t frame_total_us;    /* wall clock per frame */
    int64_t logic_us;          /* cube logic + scene setup */
    int64_t rasterize_us;      /* small3dlib S3L_drawScene() */
    int64_t present_us;        /* gfx_present() */
} frame_sample_t;

/* Fixed-size circular buffer for samples. */
#define MAX_SAMPLES  600   /* 10s @ 60fps = 600 frames max */
static frame_sample_t samples[MAX_SAMPLES];
static int sample_count = 0;

static int cmp_i64(const void *a, const void *b)
{
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    return (va > vb) - (va < vb);
}

/* Median helper: sorts `arr` in place and returns its middle element - the
 * p95 lookups below run against the same array right after calling this,
 * relying on it having already been sorted here rather than sorting again
 * themselves. */
static int64_t median_of(int64_t *arr, int n)
{
    if (n == 0) return 0;
    qsort(arr, n, sizeof(int64_t), cmp_i64);
    return arr[n / 2];
}

static void cube_perf_fixture(void)
{
    /* Use the app's own enter to set up cube, scene, etc. */
    cube_enter();
    
    /* Enable partial updates for realistic measurement. */
    gfx_set_partial_clear(true);
    gfx_set_interlace(false);
    
    sample_count = 0;
}

static void cube_perf_teardown(void)
{
    cube_exit();
}

static int64_t time_logic(int32_t dt_ms)
{
    int64_t start = esp_timer_get_time();
    cube_frame(dt_ms, NULL);  /* input NULL = no UI, just logic + draw */
    return esp_timer_get_time() - start;
}

/* Reusable input struct (NULL input works for cube_frame). */
static const input_t null_input = { 0 };

void test_cube_performance_over_10s(void)
{
    cube_perf_fixture();
    
    int64_t test_start = esp_timer_get_time();
    int64_t next_frame_due = test_start;
    int frame_idx = 0;
    
    /* Run at natural frame rate (no vTaskDelay) for SAMPLE_SECONDS. */
    while (esp_timer_get_time() - test_start < SAMPLE_MS * 1000) {
        if (frame_idx >= MAX_SAMPLES) break;
        
        int64_t frame_start = esp_timer_get_time();
        int64_t dt_ms = (frame_start - next_frame_due) / 1000;
        if (dt_ms < 0) dt_ms = 1;
        if (dt_ms > 250) dt_ms = 250;
        next_frame_due += dt_ms * 1000;
        
        /* --- LOGIC PHASE --- */
        int64_t logic_start = esp_timer_get_time();
        
        /* Manually do what cube_frame does, but timed: */
        elapsed_ms += dt_ms;
        cube.transform.rotation.y =
            (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_Y_MS) % S3L_F);
        cube.transform.rotation.x =
            (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_X_MS) % S3L_F);
        
        gfx_set_partial_clear(true);
        gfx_clear(gfx_rgb(BACKGROUND_RGB));
        
        int64_t logic_end = esp_timer_get_time();
        
        /* --- RASTERIZE PHASE --- */
        int64_t raster_start = esp_timer_get_time();
        
        frame_x0 = GFX_WIDTH;
        frame_y0 = GFX_HEIGHT;
        frame_x1 = 0;
        frame_y1 = 0;
        
        S3L_newFrame();
        S3L_drawScene(scene);  /* calls shade_pixel for every covered pixel */
        
        int64_t raster_end = esp_timer_get_time();
        
        if (frame_x1 > frame_x0 && frame_y1 > frame_y0) {
            gfx_mark_dirty(frame_x0, frame_y0, frame_x1 - frame_x0, frame_y1 - frame_y0);
        }
        
        /* --- PRESENT PHASE --- */
        int64_t present_start = esp_timer_get_time();
        gfx_present();
        int64_t present_end = esp_timer_get_time();
        
        int64_t frame_end = present_end;
        
        /* Store sample */
        samples[sample_count].frame_total_us = frame_end - frame_start;
        samples[sample_count].logic_us = logic_end - logic_start;
        samples[sample_count].rasterize_us = raster_end - raster_start;
        samples[sample_count].present_us = present_end - present_start;
        sample_count++;
        frame_idx++;
    }
    
    /* --- COMPUTE STATISTICS --- */
    if (sample_count == 0) {
        ESP_LOGE(TAG, "No frames captured!");
        cube_perf_teardown();
        TEST_FAIL();
        return;
    }
    
    int64_t total_min = INT64_MAX, total_max = 0, total_sum = 0;
    int64_t logic_min = INT64_MAX, logic_max = 0, logic_sum = 0;
    int64_t rast_min = INT64_MAX, rast_max = 0, rast_sum = 0;
    int64_t pres_min = INT64_MAX, pres_max = 0, pres_sum = 0;
    
    int64_t totals[MAX_SAMPLES];
    int64_t logics[MAX_SAMPLES];
    int64_t rasts[MAX_SAMPLES];
    int64_t press[MAX_SAMPLES];
    
    for (int i = 0; i < sample_count; i++) {
        int64_t t = samples[i].frame_total_us;
        int64_t l = samples[i].logic_us;
        int64_t r = samples[i].rasterize_us;
        int64_t p = samples[i].present_us;
        
        totals[i] = t;
        logics[i] = l;
        rasts[i] = r;
        press[i] = p;
        
        if (t < total_min) total_min = t;
        if (t > total_max) total_max = t;
        total_sum += t;
        
        if (l < logic_min) logic_min = l;
        if (l > logic_max) logic_max = l;
        logic_sum += l;
        
        if (r < rast_min) rast_min = r;
        if (r > rast_max) rast_max = r;
        rast_sum += r;
        
        if (p < pres_min) pres_min = p;
        if (p > pres_max) pres_max = p;
        pres_sum += p;
    }
    
    int64_t total_avg = total_sum / sample_count;
    int64_t logic_avg = logic_sum / sample_count;
    int64_t rast_avg = rast_sum / sample_count;
    int64_t pres_avg = pres_sum / sample_count;
    
    int64_t total_med = median_of(totals, sample_count);
    int64_t logic_med = median_of(logics, sample_count);
    int64_t rast_med = median_of(rasts, sample_count);
    int64_t pres_med = median_of(press, sample_count);
    
    /* P95 = 95th percentile */
    int64_t total_p95 = totals[(sample_count * 95) / 100];
    int64_t logic_p95 = logics[(sample_count * 95) / 100];
    int64_t rast_p95 = rasts[(sample_count * 95) / 100];
    int64_t pres_p95 = press[(sample_count * 95) / 100];
    
    /* --- OUTPUT MARKDOWN REPORT --- */
    char report_path[128];
    snprintf(report_path, sizeof(report_path),
             "/spiffs/cube_perf_%lld.md", (long long)(test_start / 1000000));
    
    FILE *f = fopen(report_path, "w");
    if (f) {
        fprintf(f, "# Cube App Performance Report\n\n");
        fprintf(f, "Captured: %lld frames over %d seconds\n\n", (long long)sample_count, SAMPLE_SECONDS);
        
        fprintf(f, "## Frame Total (wall clock)\n");
        fprintf(f, "| Stat | us | fps |\n");
        fprintf(f, "|---|---:|---:|\n");
        fprintf(f, "| Min | %lld | %.1f |\n", (long long)total_min, 1000000.0 / total_min);
        fprintf(f, "| Max | %lld | %.1f |\n", (long long)total_max, 1000000.0 / total_max);
        fprintf(f, "| Average | %lld | %.1f |\n", (long long)total_avg, 1000000.0 / total_avg);
        fprintf(f, "| Median | %lld | %.1f |\n", (long long)total_med, 1000000.0 / total_med);
        fprintf(f, "| P95 | %lld | %.1f |\n\n", (long long)total_p95, 1000000.0 / total_p95);
        
        fprintf(f, "## Breakdown\n");
        fprintf(f, "| Phase | Min | Max | Avg | Median | P95 |\n");
        fprintf(f, "|---|---:|---:|---:|---:|---:|\n");
        fprintf(f, "| Logic | %lld | %lld | %lld | %lld | %lld |\n",
                (long long)logic_min, (long long)logic_max, (long long)logic_avg,
                (long long)logic_med, (long long)logic_p95);
        fprintf(f, "| Rasterize | %lld | %lld | %lld | %lld | %lld |\n",
                (long long)rast_min, (long long)rast_max, (long long)rast_avg,
                (long long)rast_med, (long long)rast_p95);
        fprintf(f, "| Present | %lld | %lld | %lld | %lld | %lld |\n",
                (long long)pres_min, (long long)pres_max, (long long)pres_avg,
                (long long)pres_med, (long long)pres_p95);
        fprintf(f, "| **Total** | %lld | %lld | %lld | %lld | %lld |\n\n",
                (long long)total_min, (long long)total_max, (long long)total_avg,
                (long long)total_med, (long long)total_p95);
        
        fprintf(f, "## Frame Budget vs Target (60 fps = 16667 us)\n");
        fprintf(f, "| Phase | Budget %% (avg) |\n");
        fprintf(f, "|---|---:|\n");
        fprintf(f, "| Logic | %.1f%% |\n", (double)logic_avg / 16667.0 * 100.0);
        fprintf(f, "| Rasterize | %.1f%% |\n", (double)rast_avg / 16667.0 * 100.0);
        fprintf(f, "| Present | %.1f%% |\n", (double)pres_avg / 16667.0 * 100.0);
        fprintf(f, "| **Total** | %.1f%% |\n\n", (double)total_avg / 16667.0 * 100.0);
        
        fclose(f);
        ESP_LOGI(TAG, "Report written to %s", report_path);
    } else {
        ESP_LOGW(TAG, "Could not open %s for writing (SPIFFS not mounted?)", report_path);
    }
    
    /* Also log to console for immediate visibility. */
    ESP_LOGI(TAG, "=== CUBE PERF (%d frames) ===", sample_count);
    ESP_LOGI(TAG, "Total:   avg=%lldus med=%lldus p95=%lldus  (%.1f/%.1f/%.1f fps)",
             (long long)total_avg, (long long)total_med, (long long)total_p95,
             1000000.0/total_avg, 1000000.0/total_med, 1000000.0/total_p95);
    ESP_LOGI(TAG, "Logic:   avg=%lldus med=%lldus (%.1f%%)",
             (long long)logic_avg, (long long)logic_med, (double)logic_avg/total_avg*100);
    ESP_LOGI(TAG, "Raster:  avg=%lldus med=%lldus (%.1f%%)",
             (long long)rast_avg, (long long)rast_med, (double)rast_avg/total_avg*100);
    ESP_LOGI(TAG, "Present: avg=%lldus med=%lldus (%.1f%%)",
             (long long)pres_avg, (long long)pres_med, (double)pres_avg/total_avg*100);
    
    cube_perf_teardown();
    TEST_PASS();
}

/* Additional test: measure with interlace mode enabled. */
void test_cube_performance_interlaced(void)
{
    cube_perf_fixture();
    gfx_set_interlace(true);
    
    /* Quick 100-frame sample instead of 10s. */
    sample_count = 0;
    int64_t start = esp_timer_get_time();
    
    for (int i = 0; i < 100 && sample_count < MAX_SAMPLES; i++) {
        int64_t frame_start = esp_timer_get_time();
        
        int64_t dt_ms = 16;  /* ~60fps target */
        elapsed_ms += dt_ms;
        cube.transform.rotation.y = (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_Y_MS) % S3L_F);
        cube.transform.rotation.x = (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_X_MS) % S3L_F);
        
        gfx_set_partial_clear(true);
        gfx_clear(gfx_rgb(BACKGROUND_RGB));
        
        frame_x0 = GFX_WIDTH; frame_y0 = GFX_HEIGHT; frame_x1 = 0; frame_y1 = 0;
        S3L_newFrame();
        S3L_drawScene(scene);
        if (frame_x1 > frame_x0 && frame_y1 > frame_y0) {
            gfx_mark_dirty(frame_x0, frame_y0, frame_x1 - frame_x0, frame_y1 - frame_y0);
        }
        gfx_present();
        
        samples[sample_count].frame_total_us = esp_timer_get_time() - frame_start;
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
    RUN_TEST(test_cube_performance_interlaced);
}

#else  /* !DEVICE_BUILD */

void run_cube_perf_suite(void) { }

#endif  /* DEVICE_BUILD */

SUITE_REGISTER(run_cube_perf_suite);
