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
#define BOOT_ANIM_MS 5800

/* the axes grow out of the origin */
#define BOOT_ANIM_AXES_MS 450

#define BOOT_ANIM_GRID_START_MS 150
/* each ring waits for the one inside */
#define BOOT_ANIM_GRID_RING_MS 12

/* how many rings the floor draws before fading out */
#define BOOT_ANIM_GRID_RINGS 28

/* radial guide lines from the origin to the floor's own edge - 0 hides them */
#define BOOT_ANIM_GRID_SPOKES 8

/* draws each spoke as alternating dashes instead of a solid line - 0 solid, 1 dashed */
#define BOOT_ANIM_GRID_SPOKE_DASH 0

/* spokes start drawing outward from the origin */
#define BOOT_ANIM_GRID_SPOKE_START_MS 0

/* how long a spoke takes to reach the floor's own edge - 0 draws the full length instantly */
#define BOOT_ANIM_GRID_SPOKE_DRAW_MS 0

#define BOOT_ANIM_GRID_FADE_MS 300
#define BOOT_ANIM_PEN_START_MS 520
/* how long the curve takes to draw */
#define BOOT_ANIM_PEN_MS 1980

/* when the curve reaches the table's true end - independent of fade_start_ms, so it can keep drawing well past phase 1 */
#define BOOT_ANIM_PEN_FINISH_MS 4300

/* the title arrives after the curve, not during it */
#define BOOT_ANIM_TITLE_START_MS 2700

/* each letter starts this much after the one before it */
#define BOOT_ANIM_TITLE_STAGGER_MS 140

/* how long ONE letter's flight is */
#define BOOT_ANIM_TITLE_FLIGHT_MS 900

/* how far off-panel a letter starts */
#define BOOT_ANIM_TITLE_ENTRY_PX 320

/* wobble oscillations packed into the early part of the flight */
#define BOOT_ANIM_TITLE_TURNS_PHASE 229376

/* peak wobble swing right at the start */
#define BOOT_ANIM_TITLE_AMPLITUDE_PX 22

/* the small idle wave once a letter has landed */
#define BOOT_ANIM_TITLE_WAVE_AMPLITUDE_PX 11

#define BOOT_ANIM_TITLE_WAVE_PERIOD_MS 1600
#define BOOT_ANIM_TITLE_WAVE_STAGGER_MS 90
/* dissolve into the launcher */
#define BOOT_ANIM_FADE_START_MS 4300

/* one whole hue-wheel turn takes this long */
#define BOOT_ANIM_GRID_HUE_MS 2600

/* wheel positions per ring outward */
#define BOOT_ANIM_GRID_HUE_SPREAD 70

#define BOOT_ANIM_GRID_WHITEN_MAX 120
#define BOOT_ANIM_GRID_CEILING_MAX 170
#define BOOT_ANIM_GRID_MAX 70
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
#define BOOT_ANIM_GRID_STEP_Q12 1024

/* The wave's own peak amplitude - see boot_anim.h's "The wave"
 * section. Authored in meters (wave_height_m in the JSON), the same
 * as grid_step_m just above; 0 (the default for a file baked before
 * this existed - see this script's own backward-compatibility
 * comment) turns the ripple off outright, not just down. */
#define BOOT_ANIM_WAVE_HEIGHT_Q12 0

/* The wave's own crest-to-crest distance - see boot_anim.h's "The
 * wave" section. Also meters, also authored (wave_wavelength_m in
 * the JSON). */
#define BOOT_ANIM_WAVE_WAVELENGTH_Q12 3072

/* How long one full cycle takes to pass a fixed point - milliseconds,
 * authored (wave_period_ms in the JSON), the same "how long one
 * cycle takes" unit title_wave_period_ms already is for the title's
 * own wobble. */
#define BOOT_ANIM_WAVE_PERIOD_MS 3000

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
    {   520,
      {      0,   1024,  -5120 }, {      0,      0,      0 }, {    512,    512,    512 },
      {      0,      0,      0 }, {      0,      0,      0 }, {    512,    512,    512 },
      BOOT_ANIM_EASE_LINEAR },
    {  2500,
      {      0,   1024,  -5120 }, {     82,      0,      0 }, {    512,    512,    512 },
      {      0,      0,      0 }, {      0,      0,      0 }, {    512,    512,    512 },
      BOOT_ANIM_EASE_LINEAR },
    {  2700,
      {      0,   1024,  -5120 }, {     82,      0,      0 }, {    512,    512,    512 },
      {      0,      0,      0 }, {      0,      0,      0 }, {    512,    512,    512 },
      BOOT_ANIM_EASE_LINEAR },
    {  3600,
      {      0,   1024,  -5120 }, {     82,      0,      0 }, {    512,    512,    512 },
      {      0,      0,      0 }, {      0,      0,      0 }, {    717,    717,    717 },
      BOOT_ANIM_EASE_OUT },
    {  4200,
      {      0,   1024,  -5120 }, {     82,      0,      0 }, {    512,    512,    512 },
      {      0,      0,      0 }, {      0,      0,      0 }, {     56,     56,     56 },
      BOOT_ANIM_EASE_IN },
    {  4300,
      {      0,   1024,  -5120 }, {     82,      0,      0 }, {    512,    512,    512 },
      {      0,      0,      0 }, {      0,      0,      0 }, {     56,     56,     56 },
      BOOT_ANIM_EASE_LINEAR },
    {  5800,
      {   1024,   1024,  -5120 }, {    114,      0,      0 }, {    512,    512,    512 },
      {   -512,      0,      0 }, {      0,      0,      0 }, {     56,     56,     56 },
      BOOT_ANIM_EASE_OUT },
};
