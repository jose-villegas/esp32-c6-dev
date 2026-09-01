/*=============================================================================
 * app_cube - the Gouraud-shaded rotating RGB cube, as a launcher app.
 *
 * Ownership is inverted compared with a standalone renderer: this does not run
 * a frame loop, own a framebuffer, or talk to the panel. It draws into the
 * shared framebuffer when the shell calls frame(), and returns.
 *
 * Why small3dlib rather than a conventional rasterizer: it owns no
 * framebuffer (every rasterized pixel comes back through a callback) and with
 * S3L_Z_BUFFER 0 it keeps no depth buffer, resolving visibility by sorting
 * triangles back-to-front. A colour+depth rasterizer would want ~1.3 MB at
 * this resolution, against ~424 KiB of RAM on the whole chip.
 *===========================================================================*/

#include <stdint.h>
#include <stdio.h>

#include "../../app.h"
#include "../../gfx/gfx.h"
#include "../../ui/ui.h"

/* small3dlib config - must precede its include. */
#define S3L_PIXEL_FUNCTION     shade_pixel
#define S3L_RESOLUTION_X       GFX_WIDTH
#define S3L_RESOLUTION_Y       GFX_HEIGHT
#define S3L_Z_BUFFER           0   /* no depth buffer; sorting handles it */
#define S3L_SORT               1   /* back-to-front (painter's algorithm) */
#define S3L_MAX_TRIANGES_DRAWN 16  /* the cube has 12 */
#include "small3dlib.h"

/* small3dlib is fixed point: S3L_F (512) is 1.0, and is also one full turn
 * when used as an angle. */

#define CUBE_DISTANCE       (3 * S3L_F)
#define CAMERA_FOCAL_LENGTH (2 * S3L_F)

/* Milliseconds per revolution. Deliberately unequal so it tumbles rather than
 * spinning about one fixed axis. */
#define SPIN_PERIOD_Y_MS 4000
#define SPIN_PERIOD_X_MS 7000

#define BACKGROUND_RGB 0x0A0C14

static const S3L_Unit cube_vertices[]  = { S3L_CUBE_VERTICES(S3L_F) };
static const S3L_Index cube_triangles[] = { S3L_CUBE_TRIANGLES };

/* Each corner is coloured by the sign of its position: +x adds red, +y green,
 * +z blue. Interpolating those across each face gives the gradients. */
static const uint8_t cube_corner_colors[S3L_CUBE_VERTEX_COUNT][3] = {
    { 255,   0,   0 },  /* 0  right, bottom, front */
    {   0,   0,   0 },  /* 1  left,  bottom, front */
    { 255, 255,   0 },  /* 2  right, top,    front */
    {   0, 255,   0 },  /* 3  left,  top,    front */
    { 255,   0, 255 },  /* 4  right, bottom, back  */
    {   0,   0, 255 },  /* 5  left,  bottom, back  */
    { 255, 255, 255 },  /* 6  right, top,    back  */
    {   0, 255, 255 },  /* 7  left,  top,    back  */
};

static S3L_Model3D cube;
static S3L_Scene   scene;
static uint32_t    elapsed_ms;

/* The toggle this file exists to demonstrate: whether cube_frame() clears
 * the whole framebuffer every frame (like every other app) or only the
 * pixels the cube actually touches, using gfx_mark_dirty() instead of
 * relying on gfx_clear()'s implicit "everything changed". On by default -
 * the partial-clear path this enables is what the cube app is meant to
 * showcase. Flipped from inside draw_menu(), not directly by BOOT any
 * more - see menu_open below and cube_frame().
 *
 * Exposed (suite_cube_perf.c forces this true in its fixture, so a stray
 * BOOT-menu toggle left over from manual testing can never silently skew
 * a perf run). */
bool partial_updates = true;

/* Whether the BOOT-opened menu (draw_menu()) is showing instead of the
 * cube. The normal view renders only the cube and the fps counter - see
 * cube_frame()'s own comment - and everything else, right now just the
 * partial_updates toggle, lives behind BOOT in here, the same
 * one-physical-button-one-screen-level-concern split app_diagnostics.c's
 * page cycling and app_sand.c's SAND_UI_MENU/RUNNING split already use. */
static bool menu_open;

/* This frame's drawn-pixel bounds, accumulated by shade_pixel() while
 * partial_updates is on - reset to an empty range at the top of
 * cube_frame(), widened by every covered pixel small3dlib reports. */
static int frame_x0, frame_y0, frame_x1, frame_y1;

/* On-screen framerate readout - the other half of what makes the toggle
 * above worth having: main.c's own report_fps() only ever reaches a serial
 * console, so seeing partial_updates actually change anything used to mean
 * a laptop plugged in next to the board. Windowed on dt_ms rather than
 * esp_timer_get_time() like report_fps() does, so this needs nothing beyond
 * what cube_frame() is already handed. */
#define FPS_WINDOW_MS 500
static uint32_t fps_frame_count;
static uint32_t fps_window_elapsed_ms;
static double   fps_value;

/* Last ui_layout_generation() seen, so cube_frame() can tell a shell
 * orientation change happened since last frame - see its own comment for
 * why that forces a full clear rather than a partial one. */
static uint32_t last_layout_generation;

static inline uint8_t clamp_to_byte(S3L_Unit v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* Called by small3dlib for every pixel a triangle covers - the equivalent of a
 * fragment shader, running on the CPU.
 *
 * pixel->barycentric holds three weights summing to S3L_F that say how close
 * this pixel is to each corner, so using them to average the corner colours
 * produces a smooth gradient: Gouraud shading.
 *
 * Writes straight into the framebuffer rather than going through gfx_pixel(),
 * because this runs tens of thousands of times per frame and the coordinates
 * are already guaranteed on-screen by the rasterizer. */
static inline void shade_pixel(S3L_PixelInfo *pixel)
{
    const S3L_Index *corners = cube_triangles + pixel->triangleIndex * 3;
    const uint8_t *a = cube_corner_colors[corners[0]];
    const uint8_t *b = cube_corner_colors[corners[1]];
    const uint8_t *c = cube_corner_colors[corners[2]];

    const uint8_t r = clamp_to_byte(
        S3L_interpolateBarycentric(a[0], b[0], c[0], pixel->barycentric));
    const uint8_t g = clamp_to_byte(
        S3L_interpolateBarycentric(a[1], b[1], c[1], pixel->barycentric));
    const uint8_t bl = clamp_to_byte(
        S3L_interpolateBarycentric(a[2], b[2], c[2], pixel->barycentric));

    gfx_framebuffer()[pixel->y * GFX_WIDTH + pixel->x] =
        gfx_rgb(((uint32_t)r << 16) | ((uint32_t)g << 8) | bl);

    /* Only tracked in partial_updates mode - cube_frame() is the sole
     * reader, and there is no reason to pay for it on every one of the tens
     * of thousands of pixels a frame otherwise covers. cube_frame() is also
     * where the gfx_mark_dirty() call these bounds feed into lives - see its
     * own comment on why writing gfx_framebuffer() directly, as this does,
     * requires one. */
    if (partial_updates) {
        if (pixel->x < frame_x0)     { frame_x0 = pixel->x; }
        if (pixel->x + 1 > frame_x1) { frame_x1 = pixel->x + 1; }
        if (pixel->y < frame_y0)     { frame_y0 = pixel->y; }
        if (pixel->y + 1 > frame_y1) { frame_y1 = pixel->y + 1; }
    }
}

void cube_enter(void)
{
    S3L_model3DInit(cube_vertices, S3L_CUBE_VERTEX_COUNT,
                    cube_triangles, S3L_CUBE_TRIANGLE_COUNT, &cube);
    cube.transform.translation.z = CUBE_DISTANCE;

    /* S3L_sceneInit() resets the camera to defaults, so the focal length
     * override has to come after it. */
    S3L_sceneInit(&cube, 1, &scene);

    /* Enlarge by zooming rather than moving the cube closer: at this distance
     * the near plane (S3L_F/4) would clip the front faces long before the cube
     * filled the screen, and small3dlib discards triangles that cross it, so
     * faces would silently vanish. */
    scene.camera.focalLength = CAMERA_FOCAL_LENGTH;

    elapsed_ms = 0;

    /* The framebuffer is whatever the previous app left in it - the very
     * first frame back in this app, in either mode, has to clear in full.
     * gfx_invalidate() ensures the partial clear cache starts fresh.
     * partial_updates itself is deliberately left alone: a developer toggle
     * that reset every visit would defeat the point of it, same as
     * show_orientation in app_diagnostics.c. */
    gfx_invalidate();

    /* Unlike partial_updates, the readout itself starts over every visit -
     * a stale fps_value left over from a previous run would show a number
     * with nothing behind it for up to FPS_WINDOW_MS. */
    fps_frame_count = 0;
    fps_window_elapsed_ms = 0;
    fps_value = 0.0;

    /* Always re-enter on the cube view, never with the menu left open from
     * a previous visit - the same reason app_diagnostics.c resets `page` to
     * 0 here instead of leaving it wherever a past visit left it. */
    menu_open = false;

    /* Baseline for the orientation check in cube_frame() - without this, a
     * rotation that happened while some OTHER app was showing would read as
     * "changed since last frame" on the very first frame back in the cube,
     * forcing a clear that bbox_valid's own false above already forces. Not
     * wrong, just redundant with a clearer reason already stated. */
    last_layout_generation = ui_layout_generation();
}

/* mu_Color from a 0xRRGGBB value, opaque - same reason and same shape as
 * app_sand.c's own mu_color_hex(): every colour handed to a microui drawing
 * call crosses from this file's plain hex constants into mu's 8-bit form
 * exactly once, here. Kept as its own copy rather than shared - see
 * ORIENTATION_GRAVITY_X/Y's comment in app_diagnostics.c for why a small,
 * independent copy like this is not worth a shared header of its own. */
static mu_Color mu_color_hex(uint32_t rgb)
{
    return mu_color((int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF),
                    (int)(rgb & 0xFF), 255);
}

static mu_Rect draw_overlay_box(mu_Context *ctx, int w, int h)
{
    const mu_Rect box = ui_centered_rect(ui_width(), w, h, 2);
    mu_draw_rect(ctx, box, mu_color_hex(BACKGROUND_RGB));
    return box;
}

/* The persistent HUD: the cube and, over it, the fps line - nothing else
 * renders while the menu is closed (see cube_frame()). Drawn via
 * ui.c/microui exactly as app_diagnostics.c's own toggle page is - the
 * point of this app is proving that pairing ports unchanged to a renderer
 * that has nothing else in common with a settings screen.
 *
 * UI_NO_BACKGROUND is what lets the spinning cube show through everywhere
 * this window doesn't itself paint - see app_sand.c's draw_palette() for
 * the precedent. Unlike that panel's frozen sand, the cube keeps moving
 * underneath every frame, which is exactly the case ui_end()'s own comment
 * calls out: it repaints whenever "something else has already dirtied the
 * screen", so the fps line stays correctly composited over a background
 * that never stops changing, with no special handling needed here for that.
 *
 * BUT: mu_text() draws only its own ink, no background of its own. Under a
 * full gfx_clear() every frame that is invisible, because the whole screen
 * is blank before it ever draws. Under partial_updates it is not:
 * cube_frame() only erases the CUBE's own last bounding box, never this
 * box's, so when the fps line repaints - it changes shape every
 * FPS_WINDOW_MS as the digits do - whatever of the old digits the new ones
 * do not happen to overdraw was left on screen. draw_overlay_box() is the
 * fix: an opaque box behind the text, painted through the same mu command
 * list this whole module already hashes to skip unneeded repaints, so it
 * costs nothing on the (large majority of) frames where the fps value did
 * not actually change.
 *
 * UI_TEXT_OUTLINED is app_sand.c's palette-label fix for the same reason it
 * was built for: a label with no halo of its own would wash out against
 * whichever of the cube's shifting corner colours happens to sit behind it.
 * Left in place even with the box's own opaque backing - a NO_BACKGROUND
 * window is still one BOOT tap away whenever partial_updates is off, and
 * the halo costs nothing extra when the backing is already opaque. */
/* Exposed for performance testing (suite_cube_perf.c) - timed as its own
 * phase there, separate from the cube's own clear/rotate/rasterize work,
 * so the suite can compare the frame budget with and without the HUD
 * text. */
void draw_fps(const input_t *input)
{
    mu_Context *ctx = ui_context();
    ui_begin(input);
    ui_set_text_style(UI_TEXT_OUTLINED);

    if (ui_begin_screen(ctx, "Cube HUD",
                        MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                        MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        char fps_line[16];
        snprintf(fps_line, sizeof fps_line, "%.1f fps", fps_value);
        const int tw = gfx_text_width(fps_line, -1);
        const int th = gfx_text_height() + 4;
        
        mu_Rect box = draw_overlay_box(ctx, tw + 8, th);
        mu_layout_set_next(ctx, box, 0);
        mu_text(ctx, fps_line);

        mu_end_window(ctx);
    }

    ui_end(UI_NO_BACKGROUND);
}

#define MENU_BTN_W 300
#define MENU_BTN_H UI_ROW_HEIGHT
#define MENU_BTN_GAP 20

/* The BOOT-opened menu - currently just the partial_updates toggle, as one
 * centered bezel button, but the place any future option belongs rather
 * than growing the persistent HUD in draw_fps(). See cube_frame() for why
 * BOOT opens this instead of flipping the toggle directly, and menu_open's
 * own comment for the one-button-one-screen-level-concern precedent this
 * follows.
 *
 * Modeled on app_sand.c's own draw_menu(): one full-screen OPAQUE window
 * (ui_end(BACKGROUND_RGB), not UI_NO_BACKGROUND), because cube_frame() does
 * not draw the cube at all while menu_open is true - see there. That is
 * what makes this simpler than draw_fps(): no spinning cube underneath to
 * stay composited over, so none of that function's ghosting/ordering
 * concerns apply here.
 *
 * ui_set_button_style(UI_BUTTON_BEZEL) is required, not automatic - ui_begin()
 * resets the button style to UI_BUTTON_FLAT every frame (see ui.h), so a
 * bezelled button needs asking for on every frame that draws one, the same
 * as ui_launcher.c's own menu does. Both the button and the hint below it
 * are placed via mu_layout_set_next() at an absolute rect rather than
 * through mu_layout_row()'s normal top-down flow, the same reason and the
 * same trick app_sand.c's own two-button boot menu uses: a single small
 * control centered mid-screen has no natural row to sit in. */
static void draw_menu(const input_t *input)
{
    mu_Context *ctx = ui_context();
    ui_begin(input);
    ui_set_button_style(UI_BUTTON_BEZEL);

    if (ui_begin_screen(ctx, "Cube Menu",
                        MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                        MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        const int hint_h = gfx_text_height() + 4;
        const int total_h = MENU_BTN_H + MENU_BTN_GAP + hint_h;
        const int top = (ui_height() - total_h) / 2;

        char label[24];
        snprintf(label, sizeof label, "PARTIAL UPDATES: %s",
                 partial_updates ? "ON" : "OFF");

        mu_layout_set_next(ctx,
                           ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top),
                           0);
        if (mu_button(ctx, label)) {
            partial_updates = !partial_updates;

            /* Same reason cube_frame()'s BOOT handling forces this on every
             * open/close of this menu: flipping the toggle mid-visit resets
             * the partial clear cache. */
            gfx_invalidate();
        }

        mu_layout_set_next(ctx,
                           ui_centered_rect(ui_width(), MENU_BTN_W, hint_h,
                                            top + MENU_BTN_H + MENU_BTN_GAP),
                           0);
        mu_label(ctx, "BOOT to close");

        mu_end_window(ctx);
    }

    ui_end(BACKGROUND_RGB);
}

/* The three phases below are exposed (suite_cube_perf.c) so the perf suite
 * can time each on its own without ever touching small3dlib itself.
 * small3dlib.h defines real, non-static functions when included with
 * S3L_PIXEL_FUNCTION etc. set - not just declarations - so only the
 * translation unit that already includes it (this one) can call
 * S3L_newFrame()/S3L_drawScene() at all; a second #include from
 * suite_cube_perf.c would redefine those same symbols and fail to link.
 * cube_frame() below is just these three calls plus draw_fps(), so the
 * suite's with_hud runs exercise the exact same code, not a hand copy of
 * it that could drift out of sync. */
void cube_update_rotation(uint32_t dt_ms)
{
    elapsed_ms += dt_ms;

    cube.transform.rotation.y =
        (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_Y_MS) % S3L_F);
    cube.transform.rotation.x =
        (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_X_MS) % S3L_F);
}

void cube_clear_frame(void)
{
    /* gfx_set_partial_clear() delegates bounding-box erase and dirty marking
     * of previous-frame bounds directly to gfx_clear(). */
    gfx_set_partial_clear(partial_updates);
    gfx_clear(gfx_rgb(BACKGROUND_RGB));
}

void cube_rasterize_frame(void)
{
    if (partial_updates) {
        frame_x0 = GFX_WIDTH;
        frame_y0 = GFX_HEIGHT;
        frame_x1 = 0;
        frame_y1 = 0;
    }

    S3L_newFrame();       /* resets the triangle sorter */
    S3L_drawScene(scene); /* calls shade_pixel() for every covered pixel */

    if (partial_updates) {
        /* shade_pixel() wrote straight into gfx_framebuffer(), which gfx
         * cannot see - this is the one gfx_mark_dirty() call that tells it
         * what actually changed this frame. */
        if (frame_x1 > frame_x0 && frame_y1 > frame_y0) {
            gfx_mark_dirty(frame_x0, frame_y0, frame_x1 - frame_x0,
                           frame_y1 - frame_y0);
        }
    }
}

static void cube_frame(uint32_t dt_ms, const input_t *input)
{
    /* BOOT opens/closes the menu now, rather than flipping partial_updates
     * directly - the toggle moved onto its own bezel button inside
     * draw_menu(). Invalidation on open and close resets partial clear
     * tracking for the same reasons: opening replaces the framebuffer with
     * the menu's opaque screen, and closing repaints the cube from
     * scratch. */
    if (input->boot.pressed) {
        menu_open = !menu_open;
        gfx_invalidate();

        if (menu_open) {
            ui_invalidate();
        }
    }

    /* A shell orientation change moves draw_fps()'s overlay to a different
     * physical region - gfx_invalidate() ensures the next frame performs a
     * full screen wipe rather than a partial one. */
    const uint32_t layout_generation = ui_layout_generation();
    if (layout_generation != last_layout_generation) {
        last_layout_generation = layout_generation;
        gfx_invalidate();
    }

    /* Everything below is the cube view: the fps counter measures ITS
     * throughput specifically, so counting a frame that only ever drew the
     * menu would blend two unrelated numbers into one misleading reading. */
    if (menu_open) {
        draw_menu(input);
        return;
    }

    /* fps_value only actually changes once a window closes, so it reads as
     * a settled average rather than jittering with every frame's own dt_ms -
     * the same reason report_fps() in main.c windows instead of reporting
     * per frame. An occasional dt_ms of 0 (two frames landing in the same
     * millisecond) is harmless: fps_frame_count keeps counting them and the
     * window still closes once the rest add up. Only every single frame
     * landing under 1 ms, sustained, would stall it - not a real risk for a
     * scene this heavy to rasterize. */
    fps_frame_count++;
    fps_window_elapsed_ms += dt_ms;
    if (fps_window_elapsed_ms >= FPS_WINDOW_MS) {
        fps_value = (double)fps_frame_count * 1000.0 /
                    (double)fps_window_elapsed_ms;
        fps_frame_count = 0;
        fps_window_elapsed_ms = 0;
    }

    cube_update_rotation(dt_ms);
    cube_clear_frame();
    cube_rasterize_frame();
    draw_fps(input);
}

void cube_exit(void)
{
    gfx_set_partial_clear(false);
    gfx_invalidate();
}

/* Exported as the struct itself rather than a pointer to it, so the registry
 * in main.c can take its address in a static initializer. */
const app_t app_cube = {
    .name         = "3D Cube",
    .summary      = "Gouraud-shaded software rasterizer",
    .enter        = cube_enter,
    .frame        = cube_frame,
    .exit         = cube_exit,
    .home_gesture = true,
};

APP_REGISTER(app_cube);
