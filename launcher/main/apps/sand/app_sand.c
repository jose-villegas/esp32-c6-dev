/*=============================================================================
 * app_sand - falling sand, poured with a finger and steered by tilting.
 *
 * Three pieces, each of which knows nothing about the others:
 *   sand.c  the automaton   - pure, host-tested, no idea a screen exists
 *   imu.c   the QMI8658     - raw counts, no idea what they will be used for
 *   here    the wiring      - grid size, colours, axis mapping, rendering
 *
 * WHY THE GRID IS COARSER THAN THE SCREEN
 *
 * A cell per pixel would be 368 x 448 = 165 KB of grid. After the framebuffer
 * takes 322 KB of the chip's ~424 KB there is nowhere near that left, so a cell
 * is a square block of CELL x CELL pixels. At CELL = 2 the grid is 184 x 224,
 * or 41 KB, which fits comfortably and still looks like grains rather than
 * bricks. CELL must divide both 368 and 448, so the usable values are 2, 4, 8
 * and 16.
 *===========================================================================*/

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "../../app.h"
#include "../../gfx.h"
#include "../../imu.h"
#include "sand.h"
#include "tilt.h"

static const char *TAG = "sand";

#define CELL    2
#define GRID_W  (GFX_WIDTH  / CELL)
#define GRID_H  (GFX_HEIGHT / CELL)

/* How big a blob each touched frame drops, in cells. Large enough that a tap
 * is clearly a handful of sand rather than a speck. */
#define POUR_RADIUS 5

/* The eraser is wider than the pour. Removing sand is a corrective action and
 * wants to feel broad; pouring wants to feel placed. */
#define ERASE_RADIUS 8

/* How long the mode label stays up after the PWR button is pressed. Long
 * enough to read without hurrying, short enough not to sit over the sand. */
#define LABEL_MS 1800

#define LABEL_MARGIN 18
#define LABEL_SCALE   2

/* Below this the board is being held still enough that the reading is noise,
 * and letting noise through means friction is quietly unlocked the whole time
 * the app is open. */
#define SHAKE_DEADZONE 40

/* The simulation runs at a FIXED rate, independent of the framerate.
 *
 * A grain moves one cell per step, so steps-per-second is literally how fast
 * sand falls. Stepping once per frame tied that to the framerate - which was
 * survivable while the framerate was flat, and stopped being so the moment
 * partial presents made it swing between 60 and 230 fps depending on how much
 * was moving. Sand would have fallen fastest when least was happening.
 *
 * 60 Hz is around the fastest that still reads as grains rather than streaks. */
#define SIM_HZ            60
#define SIM_STEP_MS       (1000 / SIM_HZ)

/* Never run more than this many steps to catch up after a stall.
 *
 * Two jobs. It stops the classic spiral, where a long frame schedules extra
 * steps that make the next frame longer still. And it caps how far a grain can
 * travel between two things the eye sees: a grain moves one cell per step, so
 * this IS the speed limit, and four cells in a frame is enough to read as a
 * jump rather than as movement. Better to let the simulation lose a little
 * time than to teleport the sand. */
#define SIM_MAX_CATCHUP   2

static uint8_t    *grid;
static uint8_t    *dirty_rows;   /* GRID_H bytes: which rows changed */
static uint8_t    *sleep_rows;   /* GRID_H bytes: which rows can be skipped */
static sand_t      sim;
static tilt_t      tilt;
static gfx_color_t palette[SAND_LAST_SHADE + 1];
static bool        failed;
static bool        erasing;          /* PWR toggles pour and erase */
static uint32_t    label_left_ms;    /* countdown for the mode label */

/* Rolling averages, purely for the log line. */
static uint32_t frames;
static int64_t  step_us_total;
static int64_t  draw_us_total;
static int64_t  rows_redrawn_total;
/* Accumulated simulation time, in milliseconds scaled by 256. Scaled because
 * the flow rate is a fraction and a whole millisecond is too coarse a unit to
 * carry it - rounding to whole ms would make a slow flow stutter or stop. */
static uint32_t sim_accumulator_q8;
static int64_t  steps_total;

/*---------------------------------------------------------------------------
 * Sensor axes to screen axes
 *
 * The QMI8658 is soldered in some fixed orientation relative to the panel, and
 * nothing in the datasheet can tell us which - it is a board layout fact. These
 * two macros are the entire mapping, so correcting it is a one-line change.
 *
 * Determined by experiment, not from the datasheet, which describes the chip
 * and not how it was soldered down. Held upright the sensor reads about +1 g
 * on its X axis and roughly zero on Y, so the chip's X axis is the one running
 * down the screen - which is why the obvious guess (X to X, Y to Y) sent the
 * sand sideways.
 *
 * The Y axis then runs across the screen, but pointing left, hence the
 * negation. Both facts came from tilting the board and watching which way the
 * sand went; there is no way to derive them.
 *-------------------------------------------------------------------------*/
#define GRAVITY_SCREEN_X(s)  (-(s)->ay)
#define GRAVITY_SCREEN_Y(s)  ( (s)->ax)

/*---------------------------------------------------------------------------
 * Setup
 *-------------------------------------------------------------------------*/

static void build_palette(void)
{
    /* Six shades from a warm mid-tone to a pale highlight. Sand read as a flat
     * colour block until the range was widened this far - individual grains are
     * only 2 px, so the contrast between them has to do the work. */
    static const uint32_t shades[SAND_SHADE_COUNT] = {
        0xB07430, 0xC08840, 0xD09A4C, 0xDCA85C, 0xE8BA72, 0xF2CE90,
    };

    palette[SAND_EMPTY] = gfx_rgb(0x0A0C14);
    for (int i = 0; i < SAND_SHADE_COUNT; i++) {
        palette[SAND_FIRST_SHADE + i] = gfx_rgb(shades[i]);
    }
}

static void sand_enter(void)
{
    frames = 0;
    step_us_total = 0;
    draw_us_total = 0;
    rows_redrawn_total = 0;
    sim_accumulator_q8 = 0;
    steps_total = 0;
    erasing = false;
    label_left_ms = 0;
    failed = false;

    if (dirty_rows == NULL) {
        dirty_rows = malloc(GRID_H);
    }
    if (sleep_rows == NULL) {
        sleep_rows = malloc(GRID_H);
    }
    if (grid == NULL) {
        grid = malloc((size_t)GRID_W * GRID_H);
    }
    if (grid == NULL || dirty_rows == NULL || sleep_rows == NULL) {
        ESP_LOGE(TAG, "Could not allocate a %d x %d grid (%d bytes); "
                      "largest free block is %u",
                 GRID_W, GRID_H, GRID_W * GRID_H,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        failed = true;
        return;
    }

    build_palette();
    sand_init(&sim, grid, GRID_W, GRID_H, (uint32_t)esp_timer_get_time());
    sand_track_dirty_rows(&sim, dirty_rows);

    /* Without this, a screen full of motionless sand is the most expensive
     * thing the simulation can hold rather than the least - every settled
     * grain runs the whole decision path each step to conclude nothing. */
    sand_enable_sleeping(&sim, sleep_rows);
    tilt_reset(&tilt, IMU_COUNTS_PER_G);

    if (!imu_init()) {
        /* Not fatal. Without a sensor the gravity vector is a constant, so the
         * app degrades to plain downward sand rather than refusing to run. */
        ESP_LOGW(TAG, "No IMU - falling back to fixed downward gravity");
    }

    /* A starting heap, so the app is doing something the moment it opens
     * rather than presenting an empty screen and no clue what to do. */
    sand_spawn(&sim, GRID_W / 2, GRID_H / 4, GRID_W / 5);

    ESP_LOGI(TAG, "%d x %d grid, %d bytes, %d px cells",
             GRID_W, GRID_H, GRID_W * GRID_H, CELL);
}

static void sand_exit(void)
{
    /* The grid is kept between visits: it is the largest allocation the app
     * makes, and holding it means re-entry cannot fail because the heap
     * fragmented while something else was running. */
    if (frames > 0) {
        ESP_LOGI(TAG, "%lu frames, %lld sim steps, step %lld us, draw %lld us, "
                      "%lld of %d rows redrawn per frame",
                 (unsigned long)frames, (long long)steps_total,
                 (long long)(step_us_total / frames),
                 (long long)(draw_us_total / frames),
                 (long long)(rows_redrawn_total / frames), GRID_H);
    }
}

/*---------------------------------------------------------------------------
 * Drawing
 *-------------------------------------------------------------------------*/

/* Writes every cell of a row, empty ones included, so no separate clear is
 * needed - the background is simply the colour of an empty cell.
 *
 * Only rows the simulation reported as changed are touched, and each one tells
 * gfx which band it landed in. A settled pile therefore costs almost nothing
 * to draw AND almost nothing to send, which is where the real saving is: a
 * whole frame is 9.6 ms of bus time and drawing is a fraction of that. */
static void draw_dirty_rows(void)
{
    gfx_color_t *fb = gfx_framebuffer();
    int redrawn = 0;

    for (int cy = 0; cy < GRID_H; cy++) {
        if (!dirty_rows[cy]) {
            continue;
        }
        dirty_rows[cy] = 0;
        redrawn++;

        const uint8_t *row = &grid[cy * GRID_W];
        gfx_color_t   *out = fb + (cy * CELL) * GFX_WIDTH;

        for (int cx = 0; cx < GRID_W; cx++) {
            const gfx_color_t c = palette[row[cx]];
            gfx_color_t *p = out + cx * CELL;

            /* CELL is a compile-time constant, so these unroll away. */
            for (int dy = 0; dy < CELL; dy++) {
                for (int dx = 0; dx < CELL; dx++) {
                    p[dy * GFX_WIDTH + dx] = c;
                }
            }
        }

        /* Written through the raw framebuffer, so gfx cannot see it. Saying so
         * is not optional - a missed mark leaves stale pixels on the panel. */
        gfx_mark_dirty(0, cy * CELL, GFX_WIDTH, CELL);
    }

    rows_redrawn_total += redrawn;
}

/* Draws the mode label against whichever edge is currently UP.
 *
 * Up is the opposite of gravity, so the label follows the device rather than
 * the screen: turn the board on its side and the label moves to what is now
 * the top, and turns with it so it still reads the right way up. Anything else
 * looks like a bug the moment the device is not held upright.
 *
 * Snapped to one of four, because the font can only be turned in quarters -
 * a diagonal tilt picks whichever quarter it is nearest. */
static void draw_mode_label(int gx, int gy)
{
    const char *text = erasing ? "ERASE" : "POUR";
    const int   len  = (int)strlen(text);
    const int   span = len * 8 * LABEL_SCALE;
    const int   tall = 8 * LABEL_SCALE;

    /* Which edge is up. Compare magnitudes rather than an angle: whichever
     * component of gravity dominates names the axis, and its sign the edge. */
    const int ax = gx < 0 ? -gx : gx;
    const int ay = gy < 0 ? -gy : gy;

    int x, y, turn;
    if (ay >= ax) {
        if (gy >= 0) {
            turn = 0;                                  /* down is down */
            x = (GFX_WIDTH - span) / 2;
            y = LABEL_MARGIN;
        } else {
            turn = 2;                                  /* board upside down */
            x = (GFX_WIDTH + span) / 2 - 8 * LABEL_SCALE;
            y = GFX_HEIGHT - LABEL_MARGIN - tall;
        }
    } else {
        if (gx >= 0) {
            turn = 3;                                  /* down is to the right */
            x = LABEL_MARGIN;
            y = (GFX_HEIGHT + span) / 2 - 8 * LABEL_SCALE;
        } else {
            turn = 1;                                  /* down is to the left */
            x = GFX_WIDTH - LABEL_MARGIN - tall;
            y = (GFX_HEIGHT - span) / 2;
        }
    }

    gfx_text_turned(x, y, text,
                    gfx_rgb(erasing ? 0xFF8A5C : 0x9BE8C0),
                    LABEL_SCALE, turn);
}

/*---------------------------------------------------------------------------
 * Frame
 *-------------------------------------------------------------------------*/

static void sand_frame(uint32_t dt_ms, const input_t *input)
{
    if (failed) {
        gfx_clear(gfx_rgb(0x1A0C0C));
        gfx_text(20, GFX_HEIGHT / 2, "no memory for the grid", gfx_rgb(0xFF5C5C));
        return;
    }

    /* --- where is down, and how hard? --- */
    int gx = 0;
    int gy = IMU_COUNTS_PER_G;   /* the no-sensor fallback: straight down */
    int flow = 256;              /* ... at full speed */
    int jostle = 0;

    imu_sample_t sample = { 0 };
    if (imu_ready() && imu_read(&sample)) {
        /* The GYROSCOPE says how fast the board is turning, which is all it is
         * used for: it sets how quickly the filter tracks a genuine
         * reorientation. It is deliberately not what shaking is read from -
         * see tilt.h, and the note on rotating not being shaking. */
        const int rotation = imu_rotation_level(&sample);

        /* Smooth the raw vector before anything looks at it, and hand tilt the
         * through-screen axis too: without it a device lying on a table is
         * indistinguishable from one in free fall. */
        tilt_update(&tilt, GRAVITY_SCREEN_X(&sample), GRAVITY_SCREEN_Y(&sample),
                    sample.az, rotation, dt_ms);

        gx   = tilt_x(&tilt);
        gy   = tilt_y(&tilt);
        flow = tilt_strength(&tilt);

        const int shake = tilt_shake(&tilt);
        jostle = shake > SHAKE_DEADZONE ? shake : 0;
    }

    /* --- pour, or erase --- */
    if (input->power.pressed) {
        erasing = !erasing;
        label_left_ms = LABEL_MS;
        ESP_LOGI(TAG, "%s mode", erasing ? "erase" : "pour");
    }

    /* The grid under the label has to be redrawn every frame the label is up,
     * including the frame it expires - that last redraw is what actually wipes
     * it off. Marking bands dirty is not enough on its own: the sand only
     * repaints rows the simulation changed, so without this the label would
     * leave a hole in the pile. */
    if (label_left_ms > 0) {
        label_left_ms = (dt_ms >= label_left_ms) ? 0 : (label_left_ms - dt_ms);
        memset(dirty_rows, 1, (size_t)GRID_H);
    }

    if (input->down) {
        const int cx = input->x / CELL;
        const int cy = input->y / CELL;
        if (erasing) {
            sand_erase(&sim, cx, cy, ERASE_RADIUS);
        } else {
            sand_spawn(&sim, cx, cy, POUR_RADIUS);
        }
    }

    /* Log the direction whenever the NEAREST of the eight changes. Quiet when
     * the board is still, and it is what the axis mapping above was verified
     * against. The simulation itself uses the dithered direction, which changes
     * every frame by design and would be useless to log. */
    static int last_dx = 99, last_dy = 99;
    int dx, dy;
    sand_gravity_direction(gx, gy, &dx, &dy);
    if (dx != last_dx || dy != last_dy) {
        ESP_LOGI(TAG, "down is (%+d,%+d)  smoothed (%+6d,%+6d)  "
                      "raw (%+6d,%+6d)  shake %d",
                 dx, dy, gx, gy, sample.ax, sample.ay, jostle);
        last_dx = dx;
        last_dy = dy;
    }

    /* Fixed-timestep accumulator, with the rate scaled by how hard gravity is
     * pulling in the plane of the screen.
     *
     * A grain moves one cell per step whatever gravity is doing, so steps per
     * second IS the speed of the sand. Scaling them by tilt is what gives the
     * simulation a throttle instead of an on/off switch: laid flat it coasts to
     * a stop over a moment rather than freezing between one frame and the next.
     * It is also the real behaviour, since a grain on a tray tilted by theta is
     * driven by g*sin(theta). */
    const int64_t t0 = esp_timer_get_time();

    sim_accumulator_q8 += dt_ms * (uint32_t)flow;
    int steps = (int)(sim_accumulator_q8 / (SIM_STEP_MS * 256));
    if (steps > SIM_MAX_CATCHUP) {
        steps = SIM_MAX_CATCHUP;
        sim_accumulator_q8 = 0;      /* give up on the backlog */
    } else {
        sim_accumulator_q8 -= (uint32_t)steps * SIM_STEP_MS * 256;
    }

    for (int i = 0; i < steps; i++) {
        sand_step(&sim, gx, gy, jostle);
    }
    steps_total += steps;

    const int64_t t1 = esp_timer_get_time();
    draw_dirty_rows();

    /* On top of the sand, so it is never painted over. */
    if (label_left_ms > 0) {
        draw_mode_label(gx, gy);
    }

    const int64_t t2 = esp_timer_get_time();
    step_us_total += t1 - t0;
    draw_us_total += t2 - t1;
    frames++;
}

const app_t app_sand = {
    .name    = "Falling Sand",
    .summary = "Tilt to steer, touch to pour",
    .enter   = sand_enter,
    .frame   = sand_frame,
    .exit    = sand_exit,
};

APP_REGISTER(app_sand);
