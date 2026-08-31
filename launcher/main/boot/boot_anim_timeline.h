/*=============================================================================
 * GENERATED FILE - do not edit.
 *
 *     python tools/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json > main/boot/boot_anim_timeline.h
 *
 * The boot animation's timing constants and camera keyframes, edited as
 * main/boot/boot_anim_timeline.json - by hand, or via
 * tools/boot_anim_editor.html's Bake button - and turned into this header by
 * this script. See that script's own top comment for what a keyframe is and
 * boot_anim.h's boot_anim_timeline_sample() for how they are interpolated.
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

#define BOOT_ANIM_GRID_FADE_MS 300
#define BOOT_ANIM_PEN_START_MS 520
/* how long the curve takes to draw */
#define BOOT_ANIM_PEN_MS 1980

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
typedef enum {
    BOOT_ANIM_EASE_LINEAR = 0,   /* no easing - a plain ramp        */
    BOOT_ANIM_EASE_OUT    = 1,   /* tween_ease_out() - fast then settle */
    BOOT_ANIM_EASE_IN     = 2,   /* tween_ease_in() - slow then rush    */
} boot_anim_ease_t;

/* pos/rot are panel pixels and a boot_anim_sin() phase; scale is Q8
 * (256 == 1.0). `ease` says how the segment ENDING at this keyframe -
 * from the previous one - is eased; the first keyframe's is unused. Only
 * rot[0] and scale[0] are read by boot_anim.h today - see this file's own
 * top comment. */
typedef struct {
    uint32_t ms;
    int16_t  pos[2];
    int16_t  rot[3];
    int16_t  scale[3];
    uint8_t  ease;
} boot_anim_keyframe_t;

#define BOOT_ANIM_KEYFRAME_COUNT 7

static const boot_anim_keyframe_t boot_anim_keyframes[BOOT_ANIM_KEYFRAME_COUNT] = {
    {   520, {  184,  336 }, {      0,    0,    0 }, {  256,  256,  256 }, BOOT_ANIM_EASE_LINEAR },
    {  2500, {  184,  112 }, {  10559,    0,    0 }, {  256,  256,  256 }, BOOT_ANIM_EASE_LINEAR },
    {  2700, {  184,  112 }, {  10559,    0,    0 }, {  256,  256,  256 }, BOOT_ANIM_EASE_LINEAR },
    {  3600, {  184,  112 }, {  10559,    0,    0 }, {  380,  380,  380 }, BOOT_ANIM_EASE_OUT },
    {  4200, {  184,  112 }, {  10559,    0,    0 }, {   28,   28,   28 }, BOOT_ANIM_EASE_IN },
    {  4300, {  184,  112 }, {  10559,    0,    0 }, {   28,   28,   28 }, BOOT_ANIM_EASE_LINEAR },
    {  5800, {  151,  138 }, {  20936,    0,    0 }, {   28,   28,   28 }, BOOT_ANIM_EASE_OUT },
};
