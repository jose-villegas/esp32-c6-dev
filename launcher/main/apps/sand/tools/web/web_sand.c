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
 * instead of a microui panel.
 *
 * web_render() DOES call the real material_colours() (material.c) - the
 * same function app_sand.c's paint_row_n() calls - with a real edge mask, a
 * real per-cell hash, and a real LOCAL DEPTH walk (see compute_local_depth()
 * below), so liquid pools get the same depth-graded interior shading the
 * device shows. It also reproduces paint_row_n()'s own per-pixel HATCHED
 * pattern (see the shine block in web_render() below) - the travelling
 * glass shine, the woven diagonal grain, and water's foam dither (via
 * material_set_foam_phase(), advanced in web_step()) - using the same
 * material_shine_direction() and SHINE_* constants app_sand.c does. The
 * simulation AND its rendering are the genuine, unmodified article; the one
 * thing this file does not do is app_sand.c's SPARSE, dirty-row-only
 * repaint - see compute_local_depth()'s own comment for why that never
 * needed porting here.
 *===========================================================================*/
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

#include "material.h"
#include "sand.h"
#include "tilt.h"
#include "util/intmath.h"

/* Duplicated from gfx.h's GFX_WIDTH/GFX_HEIGHT, the same way palette.h
 * already duplicates them - gfx.h drags in bsp/esp-bsp.h, which this file,
 * like every other host-portable module in this app, must never include.
 * The DEVICE's own two dimensions, fixed - used only to size the one pixel
 * buffer this file ever allocates (see `pixels` below), at whichever of the
 * two products is larger (they are in fact equal, 368*448 either way, but
 * nothing here relies on that). What actually varies per web_init() call is
 * `screen_w`/`screen_h` just below - PORTRAIT uses these two numbers
 * unchanged; LANDSCAPE swaps them. */
#define DEVICE_W 368
#define DEVICE_H 448

/* Same value as imu.h's IMU_COUNTS_PER_G - duplicated rather than included,
 * for the same "no hardware header" reason as DEVICE_W/H above. Any
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

/* Same constants as app_sand.c's own (see there for the tuning history):
 * the travelling shine's period/speed, and how often water's foam dither
 * phase advances. */
#define SHINE_PERIOD   64
#define SHINE_STEP_MS  40
#define SHINE_STEP_PX   2
#define FOAM_PHASE_MS  90

static uint8_t   *grid;
static impulse_t *impulse_buf;
static uint8_t   *pixels;   /* DEVICE_W * DEVICE_H * 4 bytes, RGBA8888 - see
                              * web_render() below and web_pixels_ptr(),
                              * which is how JS gets at it without needing
                              * raw malloc/free exported across the wasm
                              * boundary. Allocated once, at the device's own
                              * fixed size, regardless of orientation - see
                              * screen_w/screen_h below for what varies. */
static uint8_t   *depth_buf; /* grid_w * grid_h bytes - see
                               * compute_local_depth() below. */
static sand_t      sim;
static tilt_t      tilt;

/* This orientation's own canvas size - DEVICE_W x DEVICE_H (portrait) or
 * DEVICE_H x DEVICE_W (landscape), set by web_init() below. Everything that
 * used to read the fixed DEVICE_W/H constants directly now reads these instead:
 * the grid's own cell count, web_render()'s row stride and bounds, and the
 * two web_screen_w()/web_screen_h() getters JS sizes its canvas from. A
 * landscape grid is genuinely landscape-shaped (grid_w > grid_h) rather
 * than a portrait grid rotated for display - see web_init()'s own comment
 * for why that is what makes landscape need no other change anywhere:
 * gravity, the tilt pad, and touch input all already work in whichever
 * axes screen_w/screen_h currently mean, unchanged. */
static int screen_w = DEVICE_W, screen_h = DEVICE_H;

static int grid_w, grid_h, cell_px;
static int brush_index;
static uint32_t sim_accumulator_q8;
static uint32_t pour_accumulator_ms;

/* This frame's gravity, cached by web_step() for web_render()'s own local-
 * depth walk (see compute_local_depth()) - which direction is "toward the
 * surface" for a liquid is a fact about gravity, not about the grid, and
 * web_render() has no gravity sample of its own to read. */
static int last_gx, last_gy = WEB_COUNTS_PER_G;

/* The travelling shine's own state - see app_sand.c's identical statics for
 * the full account of each. shine_ux_q8/shine_uy_q8 default to the same
 * (1,1) diagonal app_sand.c starts with, for a frame drawn before web_step()
 * has ever run. */
static int      shine_offset;
static uint32_t shine_elapsed_ms;
static int      shine_ux_q8 = 181;
static int      shine_uy_q8 = 181;

/* Water's foam dither clock - see material_set_foam_phase()'s own comment
 * in material.h, and FOAM_PHASE_MS above. */
static uint32_t foam_elapsed_ms;

/* True once web_init() has run - guards web_step()/web_render() against a
 * stray call before the grid exists, the same role failed's RUNNING-with-
 * no-grid path plays in app_sand.c, simplified: a web page controls its own
 * load order, so this is a cheap assert rather than a user-facing screen. */
static int ready;

/*---------------------------------------------------------------------------
 * Setup
 *-------------------------------------------------------------------------*/

/* `landscape` swaps which of DEVICE_W/DEVICE_H is screen_w vs screen_h -
 * see screen_w/screen_h's own comment above for why that alone is a
 * complete landscape mode, with nothing to rotate anywhere else: the grid
 * this allocates is genuinely (DEVICE_H/cell_px) cells wide by
 * (DEVICE_W/cell_px) cells tall in landscape, not a portrait grid drawn
 * sideways, so "down" - gravity's own +y, the tilt pad's own drag axis,
 * touch input's own y coordinate - already means the same visual direction
 * either way without this file doing any extra transform. */
EMSCRIPTEN_KEEPALIVE
int web_init(int cell_px_in, int landscape)
{
    cell_px  = cell_px_in > 0 ? cell_px_in : 2;
    screen_w = landscape ? DEVICE_H : DEVICE_W;
    screen_h = landscape ? DEVICE_W : DEVICE_H;
    grid_w   = screen_w / cell_px;
    grid_h   = screen_h / cell_px;

    free(grid);
    free(impulse_buf);
    free(depth_buf);

    grid        = malloc((size_t)grid_w * grid_h);
    depth_buf   = malloc((size_t)grid_w * grid_h);
    impulse_buf = malloc((size_t)WEB_IMPULSE_MAX * sizeof(*impulse_buf));
    if (!pixels) {
        pixels = malloc((size_t)DEVICE_W * DEVICE_H * 4);
    }
    if (!grid || !depth_buf || !pixels) {
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
int web_screen_w(void) { return screen_w; }

EMSCRIPTEN_KEEPALIVE
int web_screen_h(void) { return screen_h; }

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
    last_gx = gx;
    last_gy = gy;
    const int flow = tilt_strength(&tilt);
    const int shake = tilt_shake(&tilt);
    const int jostle = shake > 40 ? shake : 0;   /* SHAKE_DEADZONE, app_sand.c */

    /* Visual clocks, advanced every call regardless of free fall below -
     * purely cosmetic state, same as app_sand.c's own sand_frame(), which
     * updates these unconditionally too. */
    material_shine_direction(gx, gy, &shine_ux_q8, &shine_uy_q8);
    shine_elapsed_ms += dt_ms;
    if (shine_elapsed_ms >= SHINE_STEP_MS) {
        const uint32_t steps = shine_elapsed_ms / SHINE_STEP_MS;
        shine_elapsed_ms -= steps * SHINE_STEP_MS;
        shine_offset = (int)(((unsigned)shine_offset + steps * SHINE_STEP_PX)
                             & (SHINE_PERIOD - 1));
    }
    foam_elapsed_ms += dt_ms;
    material_set_foam_phase(foam_elapsed_ms / FOAM_PHASE_MS);

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
 * Render - one flat colour per cell block, full frame every call, but that
 * colour now comes from the real material_colours() (material.c) - the same
 * function app_sand.c's paint_row_n() calls - fed a real edge mask, a real
 * per-cell hash, and a real LOCAL DEPTH for liquids, so a pool of water,
 * lava, oil or acid gets the same depth-graded interior shading the device
 * shows. Still not the full paint_row_n(): the HATCHED/SPECKLED sub-pixel
 * patterns material_colours() also returns (col[1]/col[2], the glass shine
 * sweep, foam dither) are not drawn here - every cell block is filled
 * uniformly with col[0], the body colour. See this file's own top comment.
 *-------------------------------------------------------------------------*/

/* This frame's LOCAL DEPTH scale, in Q8 - see material.h's own comment on
 * material_colours()'s `depth` parameter, and app_sand.c's LOCAL DEPTH
 * block for the derivation this mirrors: `256 * len(gx,gy) / dominant_axis`,
 * the ratio that turns a raw walk-step COUNT (depth_buf[], below) into true
 * distance along the actual gravity ray. Set by compute_local_depth(),
 * read by web_render() right after. */
static unsigned local_depth_scale_q8;

/* Fills depth_buf[] with each liquid cell's LOCAL DEPTH, as a RAW STEP
 * COUNT (0..MATERIAL_LIQUID_DEPTH_BAND) - the projection into Q8 true
 * distance (local_depth_scale_q8 above) is applied once, per cell, in
 * web_render(), exactly the way app_sand.c's own local_depth_scale_q8
 * comment describes: "PROJECTED AT COMBINE TIME... a raw count is
 * gravity-agnostic".
 *
 * A MUCH simpler walk than app_sand.c's own paint_row_n() version of this
 * mechanism, for one reason: that one has to survive a SPARSE, cross-frame
 * repaint (only dirty rows redrawn, some frames apart, in whatever order
 * draw_dirty_rows() visits them) - which is the entire reason it carries a
 * double buffer, a hold-then-commit debounce, and a "which row does the
 * buffer actually describe" guard (see its own top comment for the device
 * report - a flooded 45-degree flip - each one fixed). This file redraws
 * the WHOLE grid, EVERY frame, in a single pass that always visits rows (or
 * columns) in surface-to-deep order - so every neighbour this walk reads
 * was already computed earlier in THIS SAME call, always. There is no
 * staleness to debounce against: a boundary cell's depth is 0, immediately,
 * correctly, every time, with no hold-and-see. */
static void compute_local_depth(void)
{
    const int gx = last_gx, gy = last_gy;
    const int ax = im_abs(gx), ay = im_abs(gy);
    const int dominant = ay >= ax ? ay : ax;

    if (dominant == 0) {
        /* Free fall, or no gravity sample yet - nothing is "toward the
         * surface" in any direction, so every liquid reads flat, unshaded
         * (depth 0) rather than reusing whatever direction happened to be
         * current last frame. */
        memset(depth_buf, 0, (size_t)grid_w * grid_h);
        local_depth_scale_q8 = 0;
        return;
    }
    local_depth_scale_q8 = (unsigned)(256u * (unsigned)im_len(gx, gy) / (unsigned)dominant);

    if (ay >= ax) {
        const int vdir = gy > 0 ? 1 : -1;
        const int y0 = vdir > 0 ? 0 : grid_h - 1;
        const int y_end = vdir > 0 ? grid_h : -1;

        for (int cy = y0; cy != y_end; cy += vdir) {
            const uint8_t *row = &grid[(size_t)cy * grid_w];
            const int ny = cy - vdir;
            const bool has_prev = ny >= 0 && ny < grid_h;
            const uint8_t *prev_row = has_prev ? &grid[(size_t)ny * grid_w] : NULL;
            const uint8_t *prev_depth = has_prev ? &depth_buf[(size_t)ny * grid_w] : NULL;
            uint8_t *depth_row = &depth_buf[(size_t)cy * grid_w];

            for (int cx = 0; cx < grid_w; cx++) {
                if (material_of(row[cx])->kind != KIND_LIQUID) {
                    depth_row[cx] = 0u;
                    continue;
                }
                const bool same = has_prev &&
                    CELL_MATERIAL(prev_row[cx]) == CELL_MATERIAL(row[cx]);
                if (!same) {
                    depth_row[cx] = 0u;
                    continue;
                }
                const unsigned src = prev_depth[cx];
                depth_row[cx] = (uint8_t)(src < MATERIAL_LIQUID_DEPTH_BAND
                    ? src + 1u : MATERIAL_LIQUID_DEPTH_BAND);
            }
        }
    } else {
        const int hdir = gx > 0 ? 1 : -1;
        const int x0 = hdir > 0 ? 0 : grid_w - 1;
        const int x_end = hdir > 0 ? grid_w : -1;

        for (int cy = 0; cy < grid_h; cy++) {
            const uint8_t *row = &grid[(size_t)cy * grid_w];
            uint8_t *depth_row = &depth_buf[(size_t)cy * grid_w];

            for (int cx = x0; cx != x_end; cx += hdir) {
                if (material_of(row[cx])->kind != KIND_LIQUID) {
                    depth_row[cx] = 0u;
                    continue;
                }
                const int nx = cx - hdir;
                const bool same = nx >= 0 && nx < grid_w &&
                    CELL_MATERIAL(row[nx]) == CELL_MATERIAL(row[cx]);
                if (!same) {
                    depth_row[cx] = 0u;
                    continue;
                }
                const unsigned src = depth_row[nx];
                depth_row[cx] = (uint8_t)(src < MATERIAL_LIQUID_DEPTH_BAND
                    ? src + 1u : MATERIAL_LIQUID_DEPTH_BAND);
            }
        }
    }
}

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

    compute_local_depth();

    uint8_t *rgba = pixels;

    for (int cy = 0; cy < grid_h; cy++) {
        const uint8_t *row       = &grid[(size_t)cy * grid_w];
        const uint8_t *above     = cy > 0          ? &grid[(size_t)(cy - 1) * grid_w] : NULL;
        const uint8_t *below     = cy < grid_h - 1 ? &grid[(size_t)(cy + 1) * grid_w] : NULL;
        const uint8_t *depth_row = &depth_buf[(size_t)cy * grid_w];

        const int py0 = cy * cell_px;
        const int py1 = py0 + cell_px < screen_h ? py0 + cell_px : screen_h;

        for (int cx = 0; cx < grid_w; cx++) {
            const cell_t c = row[cx];

            /* Same edge mask app_sand.c's paint_row_n() builds - see its own
             * comment (app_sand.c) for why the diagonal bits are gated
             * behind "already a cardinal edge, and water" rather than
             * always computed. */
            unsigned mask =
                ((cx > 0          && CELL_IS_EMPTY(row[cx - 1])) ? MATERIAL_EDGE_LEFT  : 0u) |
                ((cx < grid_w - 1 && CELL_IS_EMPTY(row[cx + 1])) ? MATERIAL_EDGE_RIGHT : 0u) |
                ((above != NULL   && CELL_IS_EMPTY(above[cx]))   ? MATERIAL_EDGE_UP    : 0u) |
                ((below != NULL   && CELL_IS_EMPTY(below[cx]))   ? MATERIAL_EDGE_DOWN  : 0u);

            if ((mask & MATERIAL_EDGE_CARDINAL) != 0 && CELL_MATERIAL(c) == MAT_WATER) {
                mask |=
                    ((cx > 0          && above != NULL && CELL_IS_EMPTY(above[cx - 1])) ? MATERIAL_EDGE_UP_LEFT    : 0u) |
                    ((cx < grid_w - 1 && above != NULL && CELL_IS_EMPTY(above[cx + 1])) ? MATERIAL_EDGE_UP_RIGHT   : 0u) |
                    ((cx > 0          && below != NULL && CELL_IS_EMPTY(below[cx - 1])) ? MATERIAL_EDGE_DOWN_LEFT  : 0u) |
                    ((cx < grid_w - 1 && below != NULL && CELL_IS_EMPTY(below[cx + 1])) ? MATERIAL_EDGE_DOWN_RIGHT : 0u);
            }

            const unsigned depth_raw = ((unsigned)depth_row[cx] * local_depth_scale_q8) >> 8;
            const unsigned depth_liquid = depth_raw < MATERIAL_LIQUID_DEPTH_BAND
                ? depth_raw : MATERIAL_LIQUID_DEPTH_BAND;
            /* A root borrows `depth` for its own reading - see material_
             * colours()'s own comment on this in material.h. */
            const unsigned depth = (c == MATX(MATX_ROOT))
                ? material_root_neighbours(above, row, below, cx, grid_w)
                : depth_liquid;

            gfx_color_t col[3];
            const material_pattern_t pat = material_colours(
                c, material_grain_hash(cx, cy), mask, depth, col);

            const int px0 = cx * cell_px;
            const int px1 = px0 + cell_px < screen_w ? px0 + cell_px : screen_w;

            if (pat != MATERIAL_HATCHED) {
                /* FLAT and SPECKLED both land here, same as app_sand.c's
                 * own paint_row_n(): a speckled cell already arrived with a
                 * different col[0], chosen once from this cell's own hash,
                 * so it needs no per-pixel work of its own either. */
                const uint32_t rgb = gfx_color_rgb888(col[0]);
                const uint8_t r = (uint8_t)(rgb >> 16);
                const uint8_t g = (uint8_t)(rgb >> 8);
                const uint8_t b = (uint8_t)rgb;

                for (int py = py0; py < py1; py++) {
                    uint8_t *out = rgba + ((size_t)py * screen_w + px0) * 4;
                    for (int px = px0; px < px1; px++) {
                        out[0] = r;
                        out[1] = g;
                        out[2] = b;
                        out[3] = 255;
                        out += 4;
                    }
                }
                continue;
            }

            /* MATERIAL_HATCHED - the woven diagonal grain plus the
             * travelling shine band, in SCREEN pixel coordinates so both
             * run unbroken across cell boundaries rather than restarting
             * per cell. Exact port of app_sand.c's paint_row_n() own
             * per-pixel loop (its own comment has the full derivation of
             * every term below) - only the destination (an RGBA byte
             * buffer here, the RGB565 framebuffer there) differs. */
            const uint32_t rgb0 = gfx_color_rgb888(col[0]);
            const uint32_t rgb1 = gfx_color_rgb888(col[1]);
            const uint32_t rgb2 = gfx_color_rgb888(col[2]);

            const int base = (cx + cy) * cell_px;
            const int diff = (cx - cy) * cell_px;
            const int shine_base_q8 =
                (cx * cell_px) * shine_ux_q8 + (cy * cell_px) * shine_uy_q8;

            for (int py = py0; py < py1; py++) {
                const int dy = py - py0;
                uint8_t *out = rgba + ((size_t)py * screen_w + px0) * 4;

                for (int px = px0; px < px1; px++) {
                    const int dx = px - px0;

                    const bool grain = (((base + dx + dy) & 7) == 0) ||
                                       (((diff + dx - dy) & 7) == 0);

                    const int shine_q8 =
                        shine_base_q8 + dx * shine_ux_q8 + dy * shine_uy_q8;
                    const int along = ((shine_q8 >> 8) + shine_offset)
                                      & (SHINE_PERIOD - 1);

                    const uint32_t rgb = (along < cell_px)
                        ? rgb2 : (grain ? rgb1 : rgb0);

                    out[0] = (uint8_t)(rgb >> 16);
                    out[1] = (uint8_t)(rgb >> 8);
                    out[2] = (uint8_t)rgb;
                    out[3] = 255;
                    out += 4;
                }
            }
        }
    }
}
