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
 * relying on gfx_clear()'s implicit "everything changed". Off by default -
 * same behaviour as before this existed - and flipped by a BOOT tap or the
 * checkbox draw_toggle() overlays; see cube_frame() and draw_toggle(). */
static bool partial_updates;

/* This frame's drawn-pixel bounds, accumulated by shade_pixel() while
 * partial_updates is on - reset to an empty range at the top of
 * cube_frame(), widened by every covered pixel small3dlib reports. */
static int frame_x0, frame_y0, frame_x1, frame_y1;

/* The bounds shade_pixel() reported LAST frame - what has to be erased
 * before this frame's draw, since the cube may have rotated away from part
 * of it. Only meaningful while bbox_valid; see cube_frame(). */
static int bbox_x0, bbox_y0, bbox_x1, bbox_y1;
static bool bbox_valid;

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

static void cube_enter(void)
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

    /* The framebuffer is whatever the previous app left in it, not
     * anything bbox_x0..y1 describes - the very first frame back in this
     * app, in either mode, has to clear in full. partial_updates itself is
     * deliberately left alone: a developer toggle that reset every visit
     * would defeat the point of it, same as show_orientation in
     * app_diagnostics.c. */
    bbox_valid = false;

    /* Unlike partial_updates, the readout itself starts over every visit -
     * a stale fps_value left over from a previous run would show a number
     * with nothing behind it for up to FPS_WINDOW_MS. */
    fps_frame_count = 0;
    fps_window_elapsed_ms = 0;
    fps_value = 0.0;

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

/* Lays out one full-width row `height` tall, paints it BACKGROUND_RGB, and
 * hands the same rect back to whichever mu_checkbox()/mu_text() call comes
 * next via mu_layout_set_next() - see draw_toggle()'s own comment for why a
 * row that changes needs an opaque backing of its own the UI_NO_BACKGROUND
 * window it sits in does not provide. mu_layout_set_next()'s `relative`
 * argument is 0 (ABSOLUTE): `row` already came from a real mu_layout_next()
 * call just above, in the same coordinate space that function's normal
 * row-advance path would hand back, so re-issuing it verbatim is correct -
 * the same trick app_sand.c's own palette tiles and ui_launcher.c's menu
 * buttons already use to place a widget at a rect they computed themselves. */
static mu_Rect draw_overlay_row(mu_Context *ctx, int height)
{
    mu_layout_row(ctx, 1, (int[]){ -1 }, height);
    mu_Rect row = mu_layout_next(ctx);
    mu_draw_rect(ctx, row, mu_color_hex(BACKGROUND_RGB));
    mu_layout_set_next(ctx, row, 0);
    return row;
}

/* Draws the small "partial updates" checkbox and the fps readout over
 * whatever the cube just drew, via ui.c/microui exactly as
 * app_diagnostics.c's own toggle page does - the point of this app is
 * proving that pairing ports unchanged to a renderer that has nothing else
 * in common with a settings screen.
 *
 * UI_NO_BACKGROUND is what lets the spinning cube show through everywhere
 * this window doesn't itself paint - see app_sand.c's draw_palette() for
 * the precedent. Unlike that panel's frozen sand, the cube keeps moving
 * underneath every frame, which is exactly the case ui_end()'s own comment
 * calls out: it repaints whenever "something else has already dirtied the
 * screen", so the checkbox stays correctly composited over a background
 * that never stops changing, with no special handling needed here for that.
 *
 * BUT: mu_text() and mu_checkbox()'s own label draw only their ink, no
 * background of their own (mu_checkbox()'s box icon is the one exception -
 * see mu_draw_control_frame() inside it). Under a full gfx_clear() every
 * frame that is invisible, because the whole screen is blank before either
 * one ever draws. Under partial_updates it is not: cube_frame() only erases
 * the CUBE's own last bounding box, never this row's, so when the fps line
 * below repaints - it changes shape every FPS_WINDOW_MS as the digits do -
 * whatever of the old digits the new ones do not happen to overdraw was
 * left on screen. draw_overlay_row() is the fix: an opaque box behind each
 * row, painted through the same mu command list this whole module already
 * hashes to skip unneeded repaints, so it costs nothing on the (large
 * majority of) frames where neither row's content actually changed.
 *
 * UI_TEXT_OUTLINED is app_sand.c's palette-label fix for the same reason it
 * was built for: a label with no halo of its own would wash out against
 * whichever of the cube's shifting corner colours happens to sit behind it.
 * Left in place even now that both rows carry their own backing - a
 * NO_BACKGROUND window is still one BOOT tap away whenever partial_updates
 * is off, and the halo costs nothing extra when the backing is already
 * opaque. */
static void draw_toggle(const input_t *input)
{
    mu_Context *ctx = ui_context();
    ui_begin(input);
    ui_set_text_style(UI_TEXT_OUTLINED);

    if (ui_begin_screen(ctx, "Cube Settings",
                        MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                        MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        draw_overlay_row(ctx, UI_ROW_HEIGHT);
        int on = partial_updates;
        mu_checkbox(ctx, "partial updates (or BOOT)", &on);
        partial_updates = on;

        draw_overlay_row(ctx, gfx_text_height() + 4);
        char fps_line[16];
        snprintf(fps_line, sizeof fps_line, "%.1f fps", fps_value);
        mu_text(ctx, fps_line);

        mu_end_window(ctx);
    }

    ui_end(UI_NO_BACKGROUND);
}

static void cube_frame(uint32_t dt_ms, const input_t *input)
{
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

    elapsed_ms += dt_ms;

    cube.transform.rotation.y =
        (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_Y_MS) % S3L_F);
    cube.transform.rotation.x =
        (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_X_MS) % S3L_F);

    /* The BOOT edge, read here rather than left to the checkbox alone, is
     * the other half of what this app exists to test: the same physical
     * button app_diagnostics.c pages its two screens with and app_sand.c
     * opens its palette with, driving a plain on/off instead. Forcing
     * bbox_valid false on every flip - not just cube_enter()'s first frame -
     * matters for the same reason: switching INTO partial mode has no
     * bounds from a partial-mode frame that never ran to erase by, and
     * switching OUT of it leaves gfx_clear() below to repaint everything
     * regardless, so there is nothing to lose by clearing the flag either
     * way. */
    if (input->boot.pressed) {
        partial_updates = !partial_updates;
        bbox_valid = false;
    }

    /* A shell orientation change moves draw_toggle()'s overlay to a
     * different physical region - ui_end() repaints it at its new spot, but
     * nothing tells THIS app that the OLD spot, wherever it was, still has
     * checkbox/fps pixels sitting in it outside whatever the cube's own
     * bbox_x0..y1 happens to cover. Tracking that region too would mean
     * this file learning the overlay's geometry, which is ui.c's job, not
     * this app's. Forcing a full clear instead - exactly what bbox_valid
     * false already gives the partial-mode branch below - is simpler and
     * correct: a rotation is a rare, discrete event, not a per-frame cost,
     * so paying for one full clear on exactly that frame is not something
     * a user could ever see as a hitch. */
    const uint32_t layout_generation = ui_layout_generation();
    if (layout_generation != last_layout_generation) {
        last_layout_generation = layout_generation;
        bbox_valid = false;
    }

    if (partial_updates) {
        /* Erase only what the LAST frame actually drew - not the whole
         * screen - since that is the only region that might now show a
         * stale pixel the cube's new pose does not redraw itself. Falls
         * back to a full clear exactly once, on the frame nothing valid is
         * known yet (see cube_enter() and the BOOT handling above). */
        if (bbox_valid) {
            gfx_fill_rect(bbox_x0, bbox_y0, bbox_x1 - bbox_x0,
                         bbox_y1 - bbox_y0, gfx_rgb(BACKGROUND_RGB));
        } else {
            gfx_clear(gfx_rgb(BACKGROUND_RGB));
        }
        frame_x0 = GFX_WIDTH;
        frame_y0 = GFX_HEIGHT;
        frame_x1 = 0;
        frame_y1 = 0;
    } else {
        gfx_clear(gfx_rgb(BACKGROUND_RGB));
    }

    S3L_newFrame();       /* resets the triangle sorter */
    S3L_drawScene(scene); /* calls shade_pixel() for every covered pixel */

    if (partial_updates) {
        /* shade_pixel() wrote straight into gfx_framebuffer(), which gfx
         * cannot see - this is the one gfx_mark_dirty() call that tells it
         * what actually changed this frame. The erase above already marked
         * bbox_x0..y1 dirty on its own (gfx_fill_rect() does that
         * internally), so only THIS frame's own bounds need marking here;
         * the two calls between them cover exactly the same region a single
         * union of both would have. */
        if (frame_x1 > frame_x0 && frame_y1 > frame_y0) {
            gfx_mark_dirty(frame_x0, frame_y0, frame_x1 - frame_x0,
                           frame_y1 - frame_y0);
        }
        bbox_x0 = frame_x0;
        bbox_y0 = frame_y0;
        bbox_x1 = frame_x1;
        bbox_y1 = frame_y1;
        bbox_valid = (frame_x1 > frame_x0 && frame_y1 > frame_y0);
    }

    draw_toggle(input);
}

static void cube_exit(void)
{
    /* Nothing acquired, nothing to release. */
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
