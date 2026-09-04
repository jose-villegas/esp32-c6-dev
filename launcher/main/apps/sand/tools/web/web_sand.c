/*=============================================================================
 * web_sand - the browser-facing shim that drives the real sand simulation
 * from JavaScript, compiled to WebAssembly.
 *
 * This is app_sand.c's job (start_sim()/handle_pour_input()/run_sim_steps())
 * done again for a browser instead of the panel: same simulation calls, same
 * fixed-step accumulators, same brush list and gesture radii - just with no
 * gfx.h, no microui and no IMU driver behind it, since none of those exist
 * on a web page. See the plan this was built from for why that split is
 * safe: material.c/sand.c/sand_liquid.c/sand_gas.c/sand_reactions.c/tilt.c
 * are already pure, host-portable C with no ESP-IDF dependency - the same
 * sources the host test runner already links - so this file is the only new
 * simulation-adjacent code the web build needs.
 *
 * WHAT THIS FILE DELIBERATELY DOES NOT DO
 *
 * No dirty-row tracking, no row_runs, no palette-panel hit-testing: a
 * browser canvas repaint of a 368x448 image is trivial, unlike the ESP32's
 * SPI bus, so web_render() below always redraws the whole frame rather than
 * tracking which rows changed. The HTML/JS side draws its own palette UI
 * instead of a microui panel. And web_render() paints each cell as a flat
 * block of material_palette()'s own colour - not app_sand.c's paint_row_n()
 * extras (the glass shine sweep, foam dither, local-depth liquid shading),
 * which are real device-rendering polish this demo does not reproduce. The
 * simulation itself - materials, reactions, liquids, gases, tilt - is the
 * genuine, unmodified article.
 *===========================================================================*/
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

#include "material.h"
#include "sand.h"
#include "tilt.h"

/* Duplicated from gfx.h's GFX_WIDTH/GFX_HEIGHT, the same way palette.h
 * already duplicates them - gfx.h drags in bsp/esp-bsp.h, which this file,
 * like every other host-portable module in this app, must never include. */
#define WEB_SCREEN_W 368
#define WEB_SCREEN_H 448

/* Same value as imu.h's IMU_COUNTS_PER_G - duplicated rather than included,
 * for the same "no hardware header" reason as WEB_SCREEN_W/H above. Any
 * caller feeding web_step() a gravity sample scales it against this. */
#define WEB_COUNTS_PER_G 4096

/* The same brush list app_sand.c ships (see its own brushes[] for why wood
 * is here and burning wood is not, and why steam is not a paintable
 * brush) - duplicated rather than shared, the same call palette.h's own top
 * comment makes for PALETTE_SCREEN_W/H: this is fourteen short lines, not
 * worth a shared header for one array. */
static const cell_t brushes[] = {
    CELL_MAKE(MAT_SAND, 0),  CELL_MAKE(MAT_WATER, 0),
    CELL_MAKE(MAT_STONE, 0), CELL_MAKE(MAT_GAS, 0),
    CELL_MAKE(MAT_FIRE, 0),  CELL_MAKE(MAT_WOOD, 0),
    CELL_MAKE(MAT_OIL, 0),   CELL_MAKE(MAT_LAVA, 0),
    CELL_MAKE(MAT_ACID, 0),  CELL_MAKE(MAT_GLASS, 0),
    CELL_MAKE(MAT_SNOW, 0),  CELL_MAKE(MAT_DIRT, 0),
    MATX(MATX_ICE),          MATX(MATX_PLANT),
};
#define BRUSH_COUNT ((int)(sizeof(brushes) / sizeof(brushes[0])))

/* Same gesture radii as app_sand.c's own POUR_RADIUS_PX/ERASE_RADIUS_PX/
 * ERASE_EMITTER_RADIUS_PX/DETONATE_RADIUS_PX - see that file's own (much
 * longer) comments on each for why these particular numbers, if the
 * history ever matters here too. Pixels, not cells, for the same reason: a
 * brush should stay the same physical size on screen at every quality. */
#define POUR_RADIUS_PX           10
#define ERASE_RADIUS_PX          16
#define ERASE_EMITTER_RADIUS_PX  32
#define DETONATE_RADIUS_PX       50

#define SIM_HZ           60
#define SIM_STEP_MS       (1000 / SIM_HZ)
#define SIM_MAX_CATCHUP   2

#define POUR_HZ           60
#define POUR_STEP_MS      (1000 / POUR_HZ)

/* Same three-way switch as sand_ui.h's sand_mode_t, renumbered as plain
 * ints crossing the wasm boundary rather than pulling that header in (it
 * includes app.h, an ESP32 input_t definition this file has no use for). */
#define WEB_MODE_PAINT     0
#define WEB_MODE_ERASE     1
#define WEB_MODE_DETONATE  2

#define WEB_IMPULSE_MAX  4096

static uint8_t   *grid;
static impulse_t *impulse_buf;
static uint8_t   *pixels;   /* WEB_SCREEN_W * WEB_SCREEN_H * 4 bytes, RGBA8888 -
                              * see web_render() below and web_pixels_ptr(),
                              * which is how JS gets at it without needing
                              * raw malloc/free exported across the wasm
                              * boundary. */
static sand_t      sim;
static tilt_t      tilt;

static int grid_w, grid_h, cell_px;
static int brush_index;
static uint32_t sim_accumulator_q8;
static uint32_t pour_accumulator_ms;

/* True once web_init() has run - guards web_step()/web_render() against a
 * stray call before the grid exists, the same role failed's RUNNING-with-
 * no-grid path plays in app_sand.c, simplified: a web page controls its own
 * load order, so this is a cheap assert rather than a user-facing screen. */
static int ready;

/*---------------------------------------------------------------------------
 * Setup
 *-------------------------------------------------------------------------*/

EMSCRIPTEN_KEEPALIVE
int web_init(int cell_px_in)
{
    cell_px = cell_px_in > 0 ? cell_px_in : 2;
    grid_w  = WEB_SCREEN_W / cell_px;
    grid_h  = WEB_SCREEN_H / cell_px;

    free(grid);
    free(impulse_buf);

    grid        = malloc((size_t)grid_w * grid_h);
    impulse_buf = malloc((size_t)WEB_IMPULSE_MAX * sizeof(*impulse_buf));
    if (!pixels) {
        pixels = malloc((size_t)WEB_SCREEN_W * WEB_SCREEN_H * 4);
    }
    if (!grid || !pixels) {
        ready = 0;
        return 0;
    }

    brush_index = 0;
    sim_accumulator_q8 = 0;
    pour_accumulator_ms = 0;

    sand_init(&sim, grid, grid_w, grid_h, (uint32_t)rand());
    sand_set_scatter(&sim, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&sim, SAND_DECAY_PER_MATERIAL);
    sand_set_evaporates(&sim, SAND_EVAPORATES_PER_MATERIAL);
    sand_set_soak(&sim, SAND_SOAK_PER_MATERIAL);
    sand_set_mobility(&sim, SAND_MOBILITY_PER_MATERIAL);
    /* No sand_track_dirty_rows() - see this file's own top comment. Sleeping
     * still applies: it is a real simulation-cost saving in wasm too, and
     * costs nothing this file does not already have room for. */
    if (impulse_buf) {
        sand_enable_impulses(&sim, impulse_buf, WEB_IMPULSE_MAX);
    }
    tilt_reset(&tilt, WEB_COUNTS_PER_G);

    ready = 1;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int web_grid_w(void) { return grid_w; }

EMSCRIPTEN_KEEPALIVE
int web_grid_h(void) { return grid_h; }

EMSCRIPTEN_KEEPALIVE
int web_screen_w(void) { return WEB_SCREEN_W; }

EMSCRIPTEN_KEEPALIVE
int web_screen_h(void) { return WEB_SCREEN_H; }

EMSCRIPTEN_KEEPALIVE
int web_brush_count(void) { return BRUSH_COUNT; }

EMSCRIPTEN_KEEPALIVE
void web_set_brush(int index)
{
    if (index >= 0 && index < BRUSH_COUNT) {
        brush_index = index;
    }
}

/* One representative RGB888 colour for brush `index`'s material, for the
 * HTML palette's own swatch buttons - the real palette, not a hand-copied
 * hex list that could drift from it. 0xRRGGBB packed into the low 24 bits. */
EMSCRIPTEN_KEEPALIVE
uint32_t web_brush_swatch(int index)
{
    if (index < 0 || index >= BRUSH_COUNT) {
        return 0;
    }
    return gfx_color_rgb888(material_palette()[brushes[index]]);
}

/*---------------------------------------------------------------------------
 * Gravity / tilt
 *
 * `ax`/`ay`/`az` are screen-axis gravity in WEB_COUNTS_PER_G-scaled units -
 * the same shape imu_read() hands app_sand.c's read_gravity_input(). A
 * caller with no real sensor (desktop, or a phone before/without
 * DeviceOrientation permission) should pass (0, WEB_COUNTS_PER_G, 0, 0) every
 * frame - straight down at a steady 1 g - which is exactly app_sand.c's own
 * no-IMU fallback, and tilt_update() adopts a first/steady sample exactly
 * rather than smoothing into it, so there is no startup lurch either way.
 *-------------------------------------------------------------------------*/

EMSCRIPTEN_KEEPALIVE
void web_step(uint32_t dt_ms, int ax, int ay, int az, int rotation)
{
    if (!ready) {
        return;
    }

    tilt_update(&tilt, ax, ay, az, rotation, dt_ms);

    const int gx = tilt_x(&tilt);
    const int gy = tilt_y(&tilt);
    const int flow = tilt_strength(&tilt);
    const int shake = tilt_shake(&tilt);
    const int jostle = shake > 40 ? shake : 0;   /* SHAKE_DEADZONE, app_sand.c */

    if (tilt_in_free_fall(&tilt)) {
        return;
    }

    sim_accumulator_q8 += dt_ms * (uint32_t)flow;
    int steps = (int)(sim_accumulator_q8 / (SIM_STEP_MS * 256));
    if (steps > SIM_MAX_CATCHUP) {
        steps = SIM_MAX_CATCHUP;
        sim_accumulator_q8 = 0;
    } else {
        sim_accumulator_q8 -= (uint32_t)steps * SIM_STEP_MS * 256;
    }
    for (int i = 0; i < steps; i++) {
        sand_step(&sim, gx, gy, jostle);
    }
}

/*---------------------------------------------------------------------------
 * Input - one call per frame, mirroring app_sand.c's handle_pour_input()
 *-------------------------------------------------------------------------*/

/* `mode` is WEB_MODE_PAINT/ERASE/DETONATE. `down` is whether the pointer is
 * currently held; `pressed` is true only on the frame it went down - the
 * same press/down distinction input_t gives handle_pour_input(). `source`
 * selects BRUSH_SPAWN behaviour (place one persistent emitter per press)
 * over plain pouring, for PAINT mode only. (x_px, y_px) are screen pixels,
 * same units the device's touch input uses. */
EMSCRIPTEN_KEEPALIVE
void web_input(int mode, int down, int pressed, int source,
              int x_px, int y_px, uint32_t dt_ms)
{
    if (!ready) {
        return;
    }

    if (mode == WEB_MODE_DETONATE) {
        pour_accumulator_ms = 0;
        if (pressed) {
            const int cx = x_px / cell_px;
            const int cy = y_px / cell_px;
            sand_explode(&sim, cx, cy, (DETONATE_RADIUS_PX + cell_px / 2) / cell_px);
        }
        return;
    }

    if (!down) {
        pour_accumulator_ms = 0;
        return;
    }

    if (mode == WEB_MODE_PAINT && source) {
        if (pressed) {
            const int cx = x_px / cell_px;
            const int cy = y_px / cell_px;
            sand_add_emitter(&sim, cx, cy, brushes[brush_index]);
        }
        return;
    }

    pour_accumulator_ms += dt_ms;
    int applications = (int)(pour_accumulator_ms / POUR_STEP_MS);
    if (applications > SIM_MAX_CATCHUP) {
        applications = SIM_MAX_CATCHUP;
        pour_accumulator_ms = 0;
    } else {
        pour_accumulator_ms -= (uint32_t)applications * POUR_STEP_MS;
    }

    const int cx = x_px / cell_px;
    const int cy = y_px / cell_px;
    for (int i = 0; i < applications; i++) {
        if (mode == WEB_MODE_ERASE) {
            sand_erase(&sim, cx, cy, (ERASE_RADIUS_PX + cell_px / 2) / cell_px);
            sand_remove_emitters(&sim, cx, cy,
                                 (ERASE_EMITTER_RADIUS_PX + cell_px / 2) / cell_px);
        } else {
            sand_spawn_cell(&sim, cx, cy,
                            (POUR_RADIUS_PX + cell_px / 2) / cell_px,
                            brushes[brush_index]);
        }
    }
}

EMSCRIPTEN_KEEPALIVE
void web_clear(void)
{
    if (ready) {
        sand_clear(&sim);
    }
}

/*---------------------------------------------------------------------------
 * Render - flat per-cell colour, full frame every call. See this file's own
 * top comment for what is deliberately left out.
 *-------------------------------------------------------------------------*/

/* The pixel buffer's own address, for JS to read directly out of wasm
 * memory (via HEAPU8) after each web_render() call - see web_render()'s own
 * comment for why this file owns the buffer rather than exposing raw
 * malloc()/free() across the wasm boundary. Valid only after web_init() has
 * run once; the address does not change across a later web_init() call
 * (the buffer is allocated once and reused - see web_init() above), so JS
 * only needs to read this once, right after the first init. */
EMSCRIPTEN_KEEPALIVE
uint8_t *web_pixels_ptr(void)
{
    return pixels;
}

EMSCRIPTEN_KEEPALIVE
void web_render(void)
{
    if (!ready) {
        return;
    }

    uint8_t *rgba = pixels;
    const gfx_color_t *pal = material_palette();

    for (int gy = 0; gy < grid_h; gy++) {
        const uint8_t *row = &grid[(size_t)gy * grid_w];
        const int py0 = gy * cell_px;
        const int py1 = py0 + cell_px < WEB_SCREEN_H ? py0 + cell_px : WEB_SCREEN_H;

        for (int gx = 0; gx < grid_w; gx++) {
            const uint32_t rgb = gfx_color_rgb888(pal[row[gx]]);
            const uint8_t r = (uint8_t)(rgb >> 16);
            const uint8_t g = (uint8_t)(rgb >> 8);
            const uint8_t b = (uint8_t)rgb;

            const int px0 = gx * cell_px;
            const int px1 = px0 + cell_px < WEB_SCREEN_W ? px0 + cell_px : WEB_SCREEN_W;

            for (int py = py0; py < py1; py++) {
                uint8_t *out = rgba + ((size_t)py * WEB_SCREEN_W + px0) * 4;
                for (int px = px0; px < px1; px++) {
                    out[0] = r;
                    out[1] = g;
                    out[2] = b;
                    out[3] = 255;
                    out += 4;
                }
            }
        }
    }
}
