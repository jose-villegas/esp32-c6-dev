#include <stdint.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Panel's real max resolution, from the BSP. */
#define H_RES BSP_LCD_H_RES
#define V_RES BSP_LCD_V_RES

/* The framebuffer is pushed to the panel in horizontal strips. Because the
 * strips are full width, each one is a contiguous slice of the framebuffer,
 * so they can be handed to the DMA directly with no copy. 448 / 64 = 7. */
#define STRIP_H 64

/* small3dlib config - must precede its include.
 *
 * S3L_Z_BUFFER 0 + S3L_SORT 1 is what makes this fit: visibility is resolved
 * by sorting triangles back-to-front, costing one small index array
 * (S3L_MAX_TRIANGES_DRAWN entries) instead of a full-screen depth buffer.
 * The library also owns no framebuffer of its own - it hands us every
 * rasterized pixel through S3L_PIXEL_FUNCTION. Between them, those two
 * properties free up enough of the ~424 KiB of internal RAM that we can
 * afford a genuine full-screen RGB565 framebuffer (322 KiB). */
#define S3L_PIXEL_FUNCTION drawPixel
#define S3L_RESOLUTION_X H_RES
#define S3L_RESOLUTION_Y V_RES
#define S3L_Z_BUFFER 0
#define S3L_SORT 1
#define S3L_MAX_TRIANGES_DRAWN 16
#include "small3dlib.h"

static const char *TAG = "cube_demo";

static const S3L_Unit cubeVertices[] = { S3L_CUBE_VERTICES(S3L_F) };
static const S3L_Index cubeTriangles[] = { S3L_CUBE_TRIANGLES };

/* Classic RGB colour cube: each corner's colour is its sign in x/y/z, so the
 * barycentric interpolation across each face produces smooth gradients. */
static const uint8_t cubeColors[S3L_CUBE_VERTEX_COUNT][3] = {
    { 255,   0,   0 },  /* 0  +x -y -z */
    {   0,   0,   0 },  /* 1  -x -y -z */
    { 255, 255,   0 },  /* 2  +x +y -z */
    {   0, 255,   0 },  /* 3  -x +y -z */
    { 255,   0, 255 },  /* 4  +x -y +z */
    {   0,   0, 255 },  /* 5  -x -y +z */
    { 255, 255, 255 },  /* 6  +x +y +z */
    {   0, 255, 255 },  /* 7  -x +y +z */
};

static uint16_t *g_fb;              /* full-screen RGB565, DMA-capable */
static SemaphoreHandle_t g_trans_done;

/* Per-frame diagnostics. */
static uint32_t g_pixels;
static int g_minX, g_maxX, g_minY, g_maxY;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(g_trans_done, &hp);
    return hp == pdTRUE;
}

/* The QSPI panel expects byte-swapped RGB565 (esp_lvgl_port sets
 * swap_bytes=true for it); going through the raw panel API we swap ourselves. */
static inline uint16_t rgb565_swapped(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint8_t clamp8(S3L_Unit v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

/* small3dlib calls this for every rasterized pixel. With a full framebuffer
 * there is no strip test and no discarding - every pixel it produces is kept,
 * so the scene is rasterized exactly once per frame. */
static inline void drawPixel(S3L_PixelInfo *p)
{
    g_pixels++;
    if (p->x < g_minX) g_minX = p->x;
    if (p->x > g_maxX) g_maxX = p->x;
    if (p->y < g_minY) g_minY = p->y;
    if (p->y > g_maxY) g_maxY = p->y;

    const S3L_Index *tri = cubeTriangles + p->triangleIndex * 3;
    const uint8_t *c0 = cubeColors[tri[0]];
    const uint8_t *c1 = cubeColors[tri[1]];
    const uint8_t *c2 = cubeColors[tri[2]];

    const uint8_t r = clamp8(S3L_interpolateBarycentric(c0[0], c1[0], c2[0], p->barycentric));
    const uint8_t g = clamp8(S3L_interpolateBarycentric(c0[1], c1[1], c2[1], p->barycentric));
    const uint8_t b = clamp8(S3L_interpolateBarycentric(c0[2], c1[2], c2[2], p->barycentric));

    g_fb[p->y * H_RES + p->x] = rgb565_swapped(r, g, b);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Display resolution: %d x %d", H_RES, V_RES);

    /* Counting, not binary: we queue every strip of the frame up front, so
     * several transfers can complete before we start waiting on them. A
     * binary semaphore would saturate at 1 and discard the rest, and the
     * second take would block forever. */
    g_trans_done = xSemaphoreCreateCounting(V_RES / STRIP_H + 2, 0);
    if (g_trans_done == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    const bsp_display_config_t disp_cfg = {
        .max_transfer_sz = H_RES * STRIP_H * sizeof(uint16_t),
    };
    if (bsp_display_new(&disp_cfg, &panel, &io) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init display");
        return;
    }

    /* draw_bitmap is asynchronous - it queues a DMA transfer straight out of
     * the buffer we pass, so we must not touch that memory until it drains. */
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL));

    const size_t fb_bytes = (size_t)H_RES * V_RES * sizeof(uint16_t);
    g_fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (g_fb == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u byte framebuffer (largest free DMA block %u)",
                 (unsigned)fb_bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        return;
    }
    ESP_LOGI(TAG, "Framebuffer: %u bytes, free heap now %u bytes",
             (unsigned)fb_bytes, (unsigned)esp_get_free_heap_size());

    S3L_Model3D cube;
    S3L_model3DInit(cubeVertices, S3L_CUBE_VERTEX_COUNT,
                    cubeTriangles, S3L_CUBE_TRIANGLE_COUNT, &cube);
    cube.transform.translation.z = 3 * S3L_F;   /* push away from the camera */

    S3L_Scene scene;
    S3L_sceneInit(&cube, 1, &scene);

    /* Zoom in with focal length rather than moving the cube closer: at this
     * distance the near plane (S3L_F/4) would clip the front faces long
     * before the cube filled the screen. */
    scene.camera.focalLength = 2 * S3L_F;

    const uint16_t bg = rgb565_swapped(10, 12, 20);
    const uint32_t bg2 = ((uint32_t)bg << 16) | bg;

    uint32_t frame = 0;
    int64_t window_start = esp_timer_get_time();

    while (1) {
        g_pixels = 0;
        g_minX = H_RES; g_maxX = -1; g_minY = V_RES; g_maxY = -1;

        /* Drive rotation from wall-clock time so the speed is independent of
         * how fast we happen to be rendering. S3L_F units = one full turn. */
        const int64_t ms = esp_timer_get_time() / 1000;
        cube.transform.rotation.y = (S3L_Unit)((ms * S3L_F / 4000) % S3L_F);
        cube.transform.rotation.x = (S3L_Unit)((ms * S3L_F / 7000) % S3L_F);

        const int64_t t_clear = esp_timer_get_time();
        uint32_t *fill = (uint32_t *)g_fb;
        for (int i = 0; i < H_RES * V_RES / 2; i++) {
            fill[i] = bg2;
        }

        const int64_t t_raster = esp_timer_get_time();
        S3L_newFrame();
        S3L_drawScene(scene);

        /* Push the finished frame out in full-width strips. Each strip is a
         * contiguous slice of the framebuffer, so no copying is needed. */
        const int64_t t_blit = esp_timer_get_time();
        int inFlight = 0;
        for (int y0 = 0; y0 < V_RES; y0 += STRIP_H) {
            esp_lcd_panel_draw_bitmap(panel, 0, y0, H_RES, y0 + STRIP_H,
                                      g_fb + (size_t)y0 * H_RES);
            inFlight++;
        }
        while (inFlight > 0) {
            xSemaphoreTake(g_trans_done, portMAX_DELAY);
            inFlight--;
        }
        const int64_t t_end = esp_timer_get_time();

        if (frame < 3) {
            ESP_LOGI(TAG, "frame %u: %u px, bbox x[%d..%d] y[%d..%d]",
                     (unsigned)frame, (unsigned)g_pixels, g_minX, g_maxX, g_minY, g_maxY);
            ESP_LOGI(TAG, "  clear %lld us, raster %lld us, blit %lld us",
                     (long long)(t_raster - t_clear),
                     (long long)(t_blit - t_raster),
                     (long long)(t_end - t_blit));
        }

        if (++frame % 30 == 0) {
            const int64_t now = esp_timer_get_time();
            ESP_LOGI(TAG, "%.1f fps", 30.0 * 1000000.0 / (double)(now - window_start));
            window_start = now;
        }

        vTaskDelay(1);   /* yield so the idle task can feed the watchdog */
    }
}
