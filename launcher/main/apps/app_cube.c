/*=============================================================================
 * app_cube - the Gouraud-shaded rotating RGB cube, as a launcher app.
 *
 * This is the cube_demo renderer with its ownership inverted: it no longer
 * runs a frame loop, owns a framebuffer or talks to the panel. It draws into
 * the shared framebuffer when the shell calls frame() and returns.
 *
 * Why small3dlib rather than a conventional rasterizer: it owns no
 * framebuffer (every rasterized pixel comes back through a callback) and with
 * S3L_Z_BUFFER 0 it keeps no depth buffer, resolving visibility by sorting
 * triangles back-to-front. A colour+depth rasterizer would want ~1.3 MB at
 * this resolution, against ~424 KiB of RAM on the whole chip.
 *===========================================================================*/

#include <stdint.h>

#include "../app.h"
#include "../gfx.h"

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
}

static void cube_frame(uint32_t dt_ms, const input_t *input)
{
    (void)input;   /* the cube has no controls yet */

    elapsed_ms += dt_ms;

    cube.transform.rotation.y =
        (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_Y_MS) % S3L_F);
    cube.transform.rotation.x =
        (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_X_MS) % S3L_F);

    gfx_clear(gfx_rgb(BACKGROUND_RGB));

    S3L_newFrame();       /* resets the triangle sorter */
    S3L_drawScene(scene); /* calls shade_pixel() for every covered pixel */
}

static void cube_exit(void)
{
    /* Nothing acquired, nothing to release. */
}

/* Exported as the struct itself rather than a pointer to it, so the registry
 * in main.c can take its address in a static initializer. */
const app_t app_cube = {
    .name    = "3D Cube",
    .summary = "Gouraud-shaded software rasterizer",
    .enter   = cube_enter,
    .frame   = cube_frame,
    .exit    = cube_exit,
};
