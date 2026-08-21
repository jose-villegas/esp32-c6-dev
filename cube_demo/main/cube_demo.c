/*=============================================================================
 * cube_demo - a rotating, Gouraud-shaded 3D cube on the ESP32-C6.
 *
 * WHAT THIS DRAWS
 *   A cube whose eight corners are each tinted by the sign of their x/y/z
 *   position, giving the classic "RGB colour cube". Every face is filled with
 *   a smooth gradient blended between its corner colours, and the whole thing
 *   tumbles on two axes.
 *
 * THE PIPELINE, ONCE PER FRAME
 *   1. SPIN      - update the cube's orientation from the wall clock.
 *   2. CLEAR     - fill the framebuffer with the background colour.
 *   3. RASTERIZE - small3dlib transforms, projects, sorts and fills the
 *                  triangles, calling shade_pixel() for each covered pixel.
 *   4. PRESENT   - DMA the finished framebuffer out to the panel.
 *
 * WHY IT FITS IN MEMORY (the interesting part)
 *   This board has NO PSRAM. All we get is ~424 KiB of internal SRAM, and the
 *   display is 368x448. A conventional software rasterizer keeps a colour
 *   buffer AND a depth buffer per pixel - at 8 bytes/pixel that is ~1.3 MB,
 *   about three times more memory than the chip physically has.
 *
 *   small3dlib avoids both costs:
 *     - It owns no framebuffer. It hands every rasterized pixel back to us
 *       through S3L_PIXEL_FUNCTION and lets us decide where it goes.
 *     - Configured with S3L_Z_BUFFER 0 it keeps no depth buffer at all;
 *       S3L_SORT 1 resolves visibility by drawing triangles back-to-front,
 *       which costs a small array of triangle indices instead.
 *
 *   That frees enough room to afford ONE full-screen RGB565 framebuffer
 *   (322 KiB), still leaving ~109 KiB headroom. Back-to-front sorting is not
 *   pixel-exact the way a depth buffer is - it cannot resolve intersecting
 *   geometry - but for a convex solid like a cube it is exactly right.
 *
 * TWO HARDWARE GOTCHAS WORTH KNOWING
 *   - esp_lcd_panel_draw_bitmap() is ASYNCHRONOUS. It queues a DMA transfer
 *     that reads directly out of the buffer you hand it. Touching that memory
 *     before the transfer completes does not fail loudly; it silently shreds
 *     the image, because the CPU and the DMA race each other down the buffer.
 *   - The completion semaphore must be COUNTING, not binary. A whole frame's
 *     worth of strips is queued before any is awaited, so several can finish
 *     first; a binary semaphore saturates at one and discards the rest, and
 *     the next wait blocks forever.
 *===========================================================================*/

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

/*-----------------------------------------------------------------------------
 * Display geometry
 *---------------------------------------------------------------------------*/

/* Taken from the board support package rather than hardcoded, so this stays
 * correct if the panel or board revision changes. */
#define SCREEN_WIDTH   BSP_LCD_H_RES   /* 368 */
#define SCREEN_HEIGHT  BSP_LCD_V_RES   /* 448 */

/* The finished frame is sent to the panel in horizontal bands rather than one
 * huge transfer, which keeps each DMA descriptor chain modest.
 *
 * Each band spans the FULL width of the screen, which matters: it means a band
 * is a single contiguous run of memory inside the framebuffer, so we can point
 * the DMA straight at it instead of copying rows out first.
 *
 * 448 rows / 64 = 7 bands exactly, so no partial band to special-case. */
#define STRIP_HEIGHT   64
#define STRIP_COUNT    (SCREEN_HEIGHT / STRIP_HEIGHT)

/*-----------------------------------------------------------------------------
 * small3dlib configuration
 *
 * These macros MUST be defined before including the library - it is a single
 * header that compiles its implementation into whichever file includes it,
 * and it reads this configuration as it goes.
 *---------------------------------------------------------------------------*/

/* The function the rasterizer calls for every pixel it produces. */
#define S3L_PIXEL_FUNCTION    shade_pixel

/* Render at the panel's true resolution. */
#define S3L_RESOLUTION_X      SCREEN_WIDTH
#define S3L_RESOLUTION_Y      SCREEN_HEIGHT

/* 0 = no depth buffer at all. See the memory discussion at the top. */
#define S3L_Z_BUFFER          0

/* 1 = resolve visibility by sorting triangles back-to-front (painter's
 * algorithm). This is what substitutes for the depth buffer we just gave up. */
#define S3L_SORT              1

/* Upper bound on triangles the sorter can hold in one frame. The cube has 12;
 * 16 leaves a little room without wasting memory. */
#define S3L_MAX_TRIANGES_DRAWN 16

#include "small3dlib.h"

/* small3dlib works in fixed point. S3L_F (512) represents 1.0, and also
 * represents one full turn when used as an angle. So "3 * S3L_F" reads as
 * "3.0 units", and a rotation value of S3L_F / 4 is a quarter turn. */

/*-----------------------------------------------------------------------------
 * Scene tuning
 *---------------------------------------------------------------------------*/

/* How far in front of the camera the cube sits. The cube is 1.0 units across,
 * so 3.0 units away comfortably clears the near plane. */
#define CUBE_DISTANCE       (3 * S3L_F)

/* We enlarge the cube by zooming the camera rather than by moving the cube
 * closer or scaling it up. Both of those would push the cube's front face
 * through the near clip plane (S3L_F / 4) long before it filled the screen,
 * and small3dlib's default near-plane policy discards any triangle that
 * crosses it - faces would simply vanish. Changing focal length magnifies the
 * projection without moving any geometry, so nothing can cross the plane. */
#define CAMERA_FOCAL_LENGTH (2 * S3L_F)

/* Seconds per full revolution on each axis. Deliberately not equal, so the
 * cube tumbles instead of spinning about a single fixed axis. */
#define SPIN_PERIOD_Y_MS    4000
#define SPIN_PERIOD_X_MS    7000

/* Background colour behind the cube (a dark blue-grey). */
#define BACKGROUND_R        10
#define BACKGROUND_G        12
#define BACKGROUND_B        20

/*-----------------------------------------------------------------------------
 * Logging cadence
 *---------------------------------------------------------------------------*/

#define DIAGNOSTIC_FRAMES   3    /* detailed per-stage timing for the first N */
#define FPS_REPORT_EVERY    30   /* average and report once per N frames */

static const char *TAG = "cube_demo";

/*-----------------------------------------------------------------------------
 * Geometry: the cube
 *
 * small3dlib ships the vertex positions and triangle indices of a unit cube as
 * macros, so we only have to supply its size and our own per-corner colours.
 *---------------------------------------------------------------------------*/

/* 8 corners, each 3 coordinates, forming a cube 1.0 units on a side. */
static const S3L_Unit cube_vertices[] = { S3L_CUBE_VERTICES(S3L_F) };

/* 12 triangles (2 per face), each 3 indices into cube_vertices. */
static const S3L_Index cube_triangles[] = { S3L_CUBE_TRIANGLES };

/* One RGB colour per corner, assigned by the sign of that corner's position:
 * +x contributes red, +y green, +z blue. Interpolating these across each face
 * is what produces the smooth gradients. */
static const uint8_t cube_corner_colors[S3L_CUBE_VERTEX_COUNT][3] = {
    { 255,   0,   0 },  /* 0  right, bottom, front  */
    {   0,   0,   0 },  /* 1  left,  bottom, front  */
    { 255, 255,   0 },  /* 2  right, top,    front  */
    {   0, 255,   0 },  /* 3  left,  top,    front  */
    { 255,   0, 255 },  /* 4  right, bottom, back   */
    {   0,   0, 255 },  /* 5  left,  bottom, back   */
    { 255, 255, 255 },  /* 6  right, top,    back   */
    {   0, 255, 255 },  /* 7  left,  top,    back   */
};

/*-----------------------------------------------------------------------------
 * Module state
 *---------------------------------------------------------------------------*/

/* The full-screen framebuffer. It must be DMA-capable because the LCD driver
 * reads it directly. shade_pixel() writes here; present_frame() sends it. */
static uint16_t *framebuffer;

/* Signalled from the LCD driver's ISR each time one strip finishes sending. */
static SemaphoreHandle_t strip_sent;

/* Statistics gathered while rasterizing, purely for the diagnostic log. */
static struct {
    uint32_t pixels_drawn;
    int min_x, max_x, min_y, max_y;
} frame_stats;

/*-----------------------------------------------------------------------------
 * Colour helpers
 *---------------------------------------------------------------------------*/

/* Pack 8-bit RGB into the panel's pixel format.
 *
 * The panel wants RGB565 (5 bits red, 6 green, 5 blue - green gets the spare
 * bit because the eye is most sensitive to it), but byte-swapped: this QSPI
 * controller expects the high and low bytes in the opposite order to the
 * chip's native little-endian layout. When rendering through LVGL its port
 * layer does this swap for us; driving the panel directly, we must do it
 * ourselves. Getting it wrong does not fail - it just renders in wrong,
 * strangely-shifted colours. */
static inline uint16_t rgb565_for_panel(uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t packed = (uint16_t)(((r & 0xF8) << 8) |   /* top 5 bits */
                                       ((g & 0xFC) << 3) |   /* top 6 bits */
                                       ( b         >> 3));   /* top 5 bits */
    return (uint16_t)((packed >> 8) | (packed << 8));
}

/* Interpolation can overshoot slightly at triangle edges due to rounding, so
 * results are clamped before being packed into a colour channel. */
static inline uint8_t clamp_to_byte(S3L_Unit value)
{
    if (value < 0)   return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

/*-----------------------------------------------------------------------------
 * Stage 3: rasterize - the fragment shader
 *---------------------------------------------------------------------------*/

/* Called by small3dlib for every pixel covered by a triangle. This is the
 * equivalent of a fragment shader on a GPU, except it runs on the CPU and we
 * are responsible for storing the result.
 *
 * `pixel->barycentric` holds three weights describing how close this pixel is
 * to each of the triangle's three corners. They always sum to S3L_F, so using
 * them to take a weighted average of the corner colours yields a smooth
 * gradient across the face - Gouraud shading.
 *
 * Note this must be `static inline` to match the forward declaration that
 * small3dlib.h emits for S3L_PIXEL_FUNCTION. */
static inline void shade_pixel(S3L_PixelInfo *pixel)
{
    /* --- diagnostics only ------------------------------------------------ */
    frame_stats.pixels_drawn++;
    if (pixel->x < frame_stats.min_x) frame_stats.min_x = pixel->x;
    if (pixel->x > frame_stats.max_x) frame_stats.max_x = pixel->x;
    if (pixel->y < frame_stats.min_y) frame_stats.min_y = pixel->y;
    if (pixel->y > frame_stats.max_y) frame_stats.max_y = pixel->y;

    /* --- look up this triangle's three corner colours -------------------- */
    const S3L_Index *corners = cube_triangles + pixel->triangleIndex * 3;
    const uint8_t *color_a = cube_corner_colors[corners[0]];
    const uint8_t *color_b = cube_corner_colors[corners[1]];
    const uint8_t *color_c = cube_corner_colors[corners[2]];

    /* --- blend them by the barycentric weights --------------------------- */
    const uint8_t r = clamp_to_byte(S3L_interpolateBarycentric(
        color_a[0], color_b[0], color_c[0], pixel->barycentric));
    const uint8_t g = clamp_to_byte(S3L_interpolateBarycentric(
        color_a[1], color_b[1], color_c[1], pixel->barycentric));
    const uint8_t b = clamp_to_byte(S3L_interpolateBarycentric(
        color_a[2], color_b[2], color_c[2], pixel->barycentric));

    /* --- store it -------------------------------------------------------- */
    framebuffer[pixel->y * SCREEN_WIDTH + pixel->x] = rgb565_for_panel(r, g, b);
}

/*-----------------------------------------------------------------------------
 * Stage 2: clear
 *---------------------------------------------------------------------------*/

/* Wipe the previous frame. Writing two pixels at a time as a single 32-bit
 * store halves the number of memory writes; the background colour is uniform,
 * so both halves of the word are identical. (memset() cannot be used here
 * because our background is not a repeating single byte.) */
static void clear_framebuffer(void)
{
    const uint16_t background = rgb565_for_panel(BACKGROUND_R,
                                                 BACKGROUND_G,
                                                 BACKGROUND_B);
    const uint32_t pixel_pair = ((uint32_t)background << 16) | background;

    uint32_t *words = (uint32_t *)framebuffer;
    const int word_count = (SCREEN_WIDTH * SCREEN_HEIGHT) / 2;

    for (int i = 0; i < word_count; i++) {
        words[i] = pixel_pair;
    }
}

/*-----------------------------------------------------------------------------
 * Stage 4: present
 *---------------------------------------------------------------------------*/

/* Called from the LCD driver's ISR when one strip has finished transferring.
 * Returns whether a higher-priority task was woken, so FreeRTOS can yield. */
static bool IRAM_ATTR on_strip_sent(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *event,
                                    void *user_context)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(strip_sent, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

/* Send the framebuffer to the panel, then wait until every strip has actually
 * been transferred.
 *
 * The wait is not optional. These calls only QUEUE transfers that read from
 * the framebuffer via DMA; returning early would let the next frame's clear
 * start overwriting memory that is still being shifted out to the display. */
static void present_frame(esp_lcd_panel_handle_t panel)
{
    for (int y = 0; y < SCREEN_HEIGHT; y += STRIP_HEIGHT) {
        /* A full-width band starts at row y, so its pixels are contiguous
         * from this offset - the DMA can read them in place. */
        const uint16_t *strip = framebuffer + (size_t)y * SCREEN_WIDTH;

        esp_lcd_panel_draw_bitmap(panel,
                                  0, y,                              /* top-left */
                                  SCREEN_WIDTH, y + STRIP_HEIGHT,    /* bottom-right, exclusive */
                                  strip);
    }

    for (int i = 0; i < STRIP_COUNT; i++) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
}

/*-----------------------------------------------------------------------------
 * Stage 1: spin
 *---------------------------------------------------------------------------*/

/* Derive the cube's orientation from elapsed wall-clock time rather than
 * advancing it by a fixed step each frame. Frame-based rotation would speed up
 * or slow down whenever the render time changed; this keeps the motion
 * constant regardless of framerate.
 *
 * Recall S3L_F is one full turn, so (elapsed / period) * S3L_F is the angle,
 * and the modulo keeps it in range. */
static void apply_spin(S3L_Model3D *cube)
{
    const int64_t elapsed_ms = esp_timer_get_time() / 1000;

    cube->transform.rotation.y =
        (S3L_Unit)((elapsed_ms * S3L_F / SPIN_PERIOD_Y_MS) % S3L_F);
    cube->transform.rotation.x =
        (S3L_Unit)((elapsed_ms * S3L_F / SPIN_PERIOD_X_MS) % S3L_F);
}

/*-----------------------------------------------------------------------------
 * Setup
 *---------------------------------------------------------------------------*/

/* Bring up the AMOLED panel and hook up the transfer-complete callback.
 *
 * Note we use bsp_display_new() rather than bsp_display_start(): the latter
 * would also spin up LVGL, which we neither need nor want here - it would cost
 * tens of KiB of RAM that we would rather spend on the framebuffer. */
static esp_err_t start_display(esp_lcd_panel_handle_t *out_panel)
{
    esp_lcd_panel_io_handle_t io = NULL;

    const bsp_display_config_t config = {
        /* The largest single transfer we will ever ask for is one strip. */
        .max_transfer_sz = SCREEN_WIDTH * STRIP_HEIGHT * sizeof(uint16_t),
    };

    const esp_err_t err = bsp_display_new(&config, out_panel, &io);
    if (err != ESP_OK) {
        return err;
    }

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = on_strip_sent,
    };
    return esp_lcd_panel_io_register_event_callbacks(io, &callbacks, NULL);
}

/* Allocate the full-screen framebuffer in DMA-capable internal RAM. */
static bool create_framebuffer(void)
{
    const size_t bytes = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t);

    framebuffer = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (framebuffer == NULL) {
        /* Report the largest available block too - if this ever fails it is
         * far more likely to be heap fragmentation than genuine exhaustion. */
        ESP_LOGE(TAG, "Could not allocate %u byte framebuffer "
                      "(largest free DMA block is %u bytes)",
                 (unsigned)bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        return false;
    }

    ESP_LOGI(TAG, "Framebuffer: %u bytes, %u bytes of heap still free",
             (unsigned)bytes, (unsigned)esp_get_free_heap_size());
    return true;
}

/* Build the scene: one cube, pushed away from the camera, viewed through a
 * zoomed-in lens. */
static void create_scene(S3L_Model3D *cube, S3L_Scene *scene)
{
    S3L_model3DInit(cube_vertices, S3L_CUBE_VERTEX_COUNT,
                    cube_triangles, S3L_CUBE_TRIANGLE_COUNT,
                    cube);

    cube->transform.translation.z = CUBE_DISTANCE;

    /* Note S3L_sceneInit() initialises the camera to sane defaults, so this
     * must come before we override the focal length below. */
    S3L_sceneInit(cube, 1, scene);
    scene->camera.focalLength = CAMERA_FOCAL_LENGTH;
}

/*-----------------------------------------------------------------------------
 * Main loop
 *---------------------------------------------------------------------------*/

void app_main(void)
{
    ESP_LOGI(TAG, "Display resolution: %d x %d", SCREEN_WIDTH, SCREEN_HEIGHT);

    /* Counting, not binary - see the note at the top of this file. Capacity is
     * one slot per strip plus a little slack. */
    strip_sent = xSemaphoreCreateCounting(STRIP_COUNT + 2, 0);
    if (strip_sent == NULL) {
        ESP_LOGE(TAG, "Could not create the strip-transfer semaphore");
        return;
    }

    esp_lcd_panel_handle_t panel = NULL;
    if (start_display(&panel) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the display");
        return;
    }

    if (!create_framebuffer()) {
        return;
    }

    S3L_Model3D cube;
    S3L_Scene scene;
    create_scene(&cube, &scene);

    uint32_t frame_number = 0;
    int64_t fps_window_start = esp_timer_get_time();

    while (1) {
        frame_stats.pixels_drawn = 0;
        frame_stats.min_x = SCREEN_WIDTH;  frame_stats.max_x = -1;
        frame_stats.min_y = SCREEN_HEIGHT; frame_stats.max_y = -1;

        apply_spin(&cube);

        const int64_t started_clear = esp_timer_get_time();
        clear_framebuffer();

        const int64_t started_raster = esp_timer_get_time();
        S3L_newFrame();          /* resets the triangle sorter for this frame */
        S3L_drawScene(scene);    /* calls shade_pixel() for every covered pixel */

        const int64_t started_present = esp_timer_get_time();
        present_frame(panel);
        const int64_t finished = esp_timer_get_time();

        if (frame_number < DIAGNOSTIC_FRAMES) {
            ESP_LOGI(TAG, "frame %u: %u pixels, bounds x[%d..%d] y[%d..%d]",
                     (unsigned)frame_number,
                     (unsigned)frame_stats.pixels_drawn,
                     frame_stats.min_x, frame_stats.max_x,
                     frame_stats.min_y, frame_stats.max_y);
            ESP_LOGI(TAG, "  clear %lld us | rasterize %lld us | present %lld us",
                     (long long)(started_raster  - started_clear),
                     (long long)(started_present - started_raster),
                     (long long)(finished        - started_present));
        }

        if (++frame_number % FPS_REPORT_EVERY == 0) {
            const int64_t now = esp_timer_get_time();
            const double seconds = (double)(now - fps_window_start) / 1000000.0;
            ESP_LOGI(TAG, "%.1f fps", FPS_REPORT_EVERY / seconds);
            fps_window_start = now;
        }

        /* Yield briefly so lower-priority housekeeping (notably the idle task,
         * which feeds the watchdog) gets a chance to run. */
        vTaskDelay(1);
    }
}
