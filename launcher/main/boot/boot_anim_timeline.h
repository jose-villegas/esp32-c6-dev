/*=============================================================================
 * GENERATED FILE - do not edit.
 *
 *     python tools/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json > main/boot/boot_anim_timeline.h
 *
 * The boot animation's timing constants and its two keyframed 3D
 * transforms (camera, and the space the grid+curve live in), edited as
 * main/boot/boot_anim_timeline.json - by hand, or via
 * tools/boot_anim_editor.html's Bake button - and turned into this header by
 * this script. See that script's own top comment for what a keyframe is
 * (plain meters/degrees/multiplier units, converted to small3dlib's fixed
 * point right here) and boot_anim.h's boot_anim_timeline_sample() for how
 * they are interpolated.
 *===========================================================================*/
#pragma once

#include <stdint.h>

/* how long the whole animation runs, start to black */
#define BOOT_ANIM_MS 5500

/* the axes grow out of the origin */
#define BOOT_ANIM_AXES_MS 450

#define BOOT_ANIM_GRID_START_MS 320
/* each ring waits for the one inside */
#define BOOT_ANIM_GRID_RING_MS 16

/* how many rings the floor draws before fading out */
#define BOOT_ANIM_GRID_RINGS 128

/* radial guide lines from the origin to the floor's own edge - 0 hides them */
#define BOOT_ANIM_GRID_SPOKES 4

/* draws each spoke as alternating dashes instead of a solid line - 0 solid, 1 dashed */
#define BOOT_ANIM_GRID_SPOKE_DASH 1

/* spokes start drawing outward from the origin */
#define BOOT_ANIM_GRID_SPOKE_START_MS 2300

/* how long a spoke takes to reach the floor's own edge - 0 draws the full length instantly */
#define BOOT_ANIM_GRID_SPOKE_DRAW_MS 800

#define BOOT_ANIM_GRID_FADE_MS 128
/* the ripple is muted before this, then lerps up to full strength */
#define BOOT_ANIM_WAVE_IN_MS 750

/* the ripple is at full strength until this, then lerps back to muted */
#define BOOT_ANIM_WAVE_OUT_MS 5500

#define BOOT_ANIM_PEN_START_MS 520
/* how long the curve takes to draw */
#define BOOT_ANIM_PEN_MS 1980

/* when the curve reaches the table's true end - independent of fade_start_ms, so it can keep drawing well past phase 1 */
#define BOOT_ANIM_PEN_FINISH_MS 4800

/* the title arrives after the curve, not during it */
#define BOOT_ANIM_TITLE_START_MS 2450

/* each letter starts this much after the one before it */
#define BOOT_ANIM_TITLE_STAGGER_MS 170

/* how long ONE letter's flight is */
#define BOOT_ANIM_TITLE_FLIGHT_MS 500

/* how far off-panel a letter starts */
#define BOOT_ANIM_TITLE_ENTRY_PX 420

/* wobble oscillations packed into the early part of the flight */
#define BOOT_ANIM_TITLE_TURNS_PHASE 92000

/* peak wobble swing right at the start */
#define BOOT_ANIM_TITLE_AMPLITUDE_PX 24

/* the small idle wave once a letter has landed */
#define BOOT_ANIM_TITLE_WAVE_AMPLITUDE_PX 12

#define BOOT_ANIM_TITLE_WAVE_PERIOD_MS 900
#define BOOT_ANIM_TITLE_WAVE_STAGGER_MS 75
/* which typeface draws the title - 0 the 40px Computer Modern coverage atlas, 1 the shell's own 8x8 bitmap; see boot_anim.c's draw_title() */
#define BOOT_ANIM_TITLE_FONT 0

/* integer pixel multiplier the title is drawn at - 1 suits a font rasterized at its final size, ~5 is what the 8x8 bitmap needs */
#define BOOT_ANIM_TITLE_SCALE 1

/* when the idle wave starts calming back to stillness - at or past total_ms it never does, which is the default */
#define BOOT_ANIM_TITLE_WAVE_OUT_MS 3200

/* how long that calming takes, from full swing to none */
#define BOOT_ANIM_TITLE_WAVE_FADE_MS 1500

/* how far down the viewer's frame the title's own centre lands - see boot_anim.h's own comment on this section for the frame it is in */
#define BOOT_ANIM_TITLE_VIEW_Y 50

/* how far into the viewer's frame the title's own left edge starts - see boot_anim.h's own comment on this section for where the default came from */
#define BOOT_ANIM_TITLE_VIEW_X 135

/* drop shadow offset, pixels right (negative is left) - 0/0 disables it */
#define BOOT_ANIM_TITLE_SHADOW_DX 3

/* drop shadow offset, pixels down (negative is up) */
#define BOOT_ANIM_TITLE_SHADOW_DY 3

/* shadow density, 0..255 - 0 invisible, 255 solid, between is a dithered fake transparency (see gfx_fill_rect_dither() in gfx.c) */
#define BOOT_ANIM_TITLE_SHADOW_ALPHA 128

/* the photograph starts crossing in as the function crosses out */
#define BOOT_ANIM_IMAGE_START_MS 3500

/* how long that one shared crossfade takes - 0 cuts straight to the photo */
#define BOOT_ANIM_IMAGE_FADE_MS 800

/* dissolve into the launcher */
#define BOOT_ANIM_FADE_START_MS 4800

/* one whole hue-wheel turn takes this long */
#define BOOT_ANIM_GRID_HUE_MS 700

/* wheel positions per ring outward */
#define BOOT_ANIM_GRID_HUE_SPREAD 64

#define BOOT_ANIM_GRID_WHITEN_MAX 32
#define BOOT_ANIM_GRID_CEILING_MAX 96
#define BOOT_ANIM_GRID_MAX 64
/* small3dlib's S3L_Camera.focalLength - 0 is an orthographic
 * projection (see boot_anim.h's "The projection" section), any other
 * value a perspective one; S3L_F (512) is small3dlib's own "normal"
 * lens default. Authored directly in this unit - it is a lens
 * property, not a position or angle, so meters/degrees do not apply. */
#define BOOT_ANIM_CAMERA_FOCAL 512

/* The floor's ring spacing - see BOOT_ANIM_GRID_RINGS's own comment
 * in boot_anim.h. Authored in meters (grid_step_m in the JSON), like
 * a transform's own pos, and converted here the same way units() in
 * boot_anim.c does. */
#define BOOT_ANIM_GRID_STEP_Q12 410

/* The wave's own peak amplitude - see boot_anim.h's "The wave"
 * section. Authored in meters (wave_height_m in the JSON), the same
 * as grid_step_m just above; 0 (the default for a file baked before
 * this existed - see this script's own backward-compatibility
 * comment) turns the ripple off outright, not just down. */
#define BOOT_ANIM_WAVE_HEIGHT_Q12 5120

/* The wave's own crest-to-crest distance - see boot_anim.h's "The
 * wave" section. Also meters, also authored (wave_wavelength_m in
 * the JSON). */
#define BOOT_ANIM_WAVE_WAVELENGTH_Q12 20480

/* How long one full cycle takes to pass a fixed point - milliseconds,
 * authored (wave_period_ms in the JSON), the same "how long one
 * cycle takes" unit title_wave_period_ms already is for the title's
 * own wobble. */
#define BOOT_ANIM_WAVE_PERIOD_MS 800

typedef enum {
    BOOT_ANIM_EASE_LINEAR = 0,   /* no easing - a plain ramp        */
    BOOT_ANIM_EASE_OUT    = 1,   /* tween_ease_out() - fast then settle */
    BOOT_ANIM_EASE_IN     = 2,   /* tween_ease_in() - slow then rush    */
} boot_anim_ease_t;

/* Both transforms' pos/rot/scale are small3dlib fixed point (S3L_F =
 * 512 = 1.0) already - converted from the JSON's plain meters/degrees/
 * multiplier units by this script, not at runtime. `ease` says how the
 * segment ENDING at this keyframe - from the previous one - is eased;
 * the first keyframe's is unused. */
typedef struct {
    uint32_t ms;
    int32_t  camera_pos[3];
    int32_t  camera_rot[3];
    int32_t  camera_scale[3];
    int32_t  space_pos[3];
    int32_t  space_rot[3];
    int32_t  space_scale[3];
    uint8_t  ease;
} boot_anim_keyframe_t;

#define BOOT_ANIM_KEYFRAME_COUNT 7

static const boot_anim_keyframe_t boot_anim_keyframes[BOOT_ANIM_KEYFRAME_COUNT] = {
    {     0,
      {      0,      0,  -5120 }, {      0,      0,      0 }, {    512,    512,    512 },
      {   5120,      0,      0 }, {   -192,    -64,    128 }, {    512,    512,    512 },
      BOOT_ANIM_EASE_LINEAR },
    {   700,
      {      0,      0,  -5120 }, {      0,      0,      0 }, {    512,    512,    512 },
      {  -2560,      0,      0 }, {   -192,    -64,    128 }, {    512,    512,    512 },
      BOOT_ANIM_EASE_OUT },
    {  1580,
      {      0,      0,  -5120 }, {      0,      0,      0 }, {    512,    512,    512 },
      {  -2560,      0,      0 }, {   -192,     38,    128 }, {    512,    512,    512 },
      BOOT_ANIM_EASE_LINEAR },
    {  2000,
      {      0,      0,  -5120 }, {      0,      0,      0 }, {    512,    512,    512 },
      {  -2560,      0,      0 }, {   -192,     38,    128 }, {    512,    512,    512 },
      BOOT_ANIM_EASE_LINEAR },
    {  2800,
      {      0,      0,  -5120 }, {      0,      0,      0 }, {    512,    512,    512 },
      {  -2560,      0,      0 }, {   -192,      0,    128 }, {    512,    512,    512 },
      BOOT_ANIM_EASE_LINEAR },
    {  3100,
      {      0,      0,  -5120 }, {      0,      0,      0 }, {    512,    512,    512 },
      {  -1024,   4096,      0 }, {   -256,    -64,      0 }, {    512,    512,    512 },
      BOOT_ANIM_EASE_LINEAR },
    {  4300,
      {      0,      0,  -5120 }, {      0,      0,      0 }, {    512,    512,    512 },
      {  -1024,   4096,      0 }, {   -256,   -384,      0 }, {    307,    307,    307 },
      BOOT_ANIM_EASE_LINEAR },
};
