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
}

/* Draws the small "partial updates" checkbox over whatever the cube just
 * drew, via ui.c/microui exactly as app_diagnostics.c's own toggle page
 * does - the point of this app is proving that pairing ports unchanged to a
 * renderer that has nothing else in common with a settings screen.
 *
 * UI_NO_BACKGROUND is what lets the spinning cube show through everywhere
 * this window doesn't itself paint - see app_sand.c's draw_palette() for
 * the precedent. Unlike that panel's frozen sand, the cube keeps moving
 * underneath every frame, which is exactly the case ui_end()'s own comment
 * calls out: it repaints whenever "something else has already dirtied the
 * screen", so the checkbox stays correctly composited over a background
 * that never stops changing, with no special handling needed here for that.
 *
 * UI_TEXT_OUTLINED is app_sand.c's palette-label fix for the same reason it
 * was built for: a label with no halo of its own would wash out against
 * whichever of the cube's shifting corner colours happens to sit behind it. */
static void draw_toggle(const input_t *input)
{
    mu_Context *ctx = ui_context();
    ui_begin(input);
    ui_set_text_style(UI_TEXT_OUTLINED);

    if (ui_begin_screen(ctx, "Cube Settings",
                        MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                        MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        mu_layout_row(ctx, 1, (int[]){ -1 }, UI_ROW_HEIGHT);
        int on = partial_updates;
        mu_checkbox(ctx, "partial updates (or BOOT)", &on);
        partial_updates = on;

        mu_end_window(ctx);
    }

    ui_end(UI_NO_BACKGROUND);
}

static void cube_frame(uint32_t dt_ms, const input_t *input)
{
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
