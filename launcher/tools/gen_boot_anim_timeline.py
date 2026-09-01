#!/usr/bin/env python3
"""Generate main/boot/boot_anim_timeline.h - the boot animation's timing
constants and its two keyframed 3D transforms (camera, and the space the
grid+curve live in).

    python tools/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json > main/boot/boot_anim_timeline.h

WHERE THIS DATA COMES FROM

main/boot/boot_anim_timeline.json is the source of truth: hand-edited, or
edited through tools/boot_anim_editor.html's "Bake" button, which downloads a
JSON file in exactly this shape to replace it. This script never talks to
the editor directly - the two are joined only by the file format.

WHAT A KEYFRAME IS

Two full position/rotation/scale transforms - "camera" and "space" - per
keyframe, plus which millisecond it lands on and how the segment ARRIVING
at it is eased. Both transforms are authored in plain, human units:

    pos     meters (one space-unit is one meter - see boot_anim.h's own
            top comment for why the curve/grid's existing geometry already
            IS this same unit, nothing further to convert on that side)
    rot     degrees, composed Z-then-X-then-Y (small3dlib's own order -
            see S3L_Transform3D's comment in small3dlib.h - NOT X-Y-Z)
    scale   a plain multiplier, 1.0 meaning "as authored" - an untouched
            keyframe's scale is [1, 1, 1], not a magic 256 or 512

and converted to small3dlib's own fixed point (S3L_F = 512 = 1.0) here, at
bake time - see meters_to_s3l()/degrees_to_s3l()/scale_to_s3l() below - so
neither the JSON nor the editor's UI ever has to deal with that scale
directly.

boot_anim.h's boot_anim_timeline_sample() walks the generated table and
lerps both transforms' every number between the two keyframes bracketing
`now_ms`, easing the fraction with whichever of linear/ease_out/ease_in the
arriving keyframe names - the same three shapes util/tween.h already
provides, so nothing new is needed on the firmware side to interpret them.

`camera_focal` and `grid_step_m` are not per-keyframe, unlike the transforms
above: `camera_focal` is a single lens setting (small3dlib's own
S3L_Camera.focalLength - see boot_anim.h's "The projection" section for
what 0 does to it: an orthographic projection, not a second code path to
maintain); `grid_step_m` is the spacing between floor rings, authored in
meters like a transform's `pos` and converted the same way. `grid_rings`
(how many rings the floor draws before fading out) lives in `timing`
instead, since it is a plain count with nothing to convert.

The `timing` block is everything else that paces the animation but is not
a transform of the space: how fast the grid rings fade in, how the title
letters fly in and wobble, how the grid's own hue travels. Plain values,
straight through to #define - unchanged in shape from before this file
grew a second transform.

VALIDATION

Refuses to emit anything that would draw something broken (curve still being
drawn when the fade starts, keyframes out of order) the same way
gen_zeta_curve.py refuses to ship a curve that fails its own zero check.
Aesthetic-only concerns (a title letter still flying when the fade starts)
are a warning, not a refusal - unlike a broken curve, that might be exactly
what someone editing the timeline wants.
"""

import json
import sys

EASE_NAMES = ["linear", "ease_out", "ease_in"]
TRANSFORMS = ["camera", "space"]

# small3dlib's own S3L_FRACTIONS_PER_UNIT - see small3dlib.h. 1.0 in its
# fixed point.
S3L_F = 512

# Must match BOOT_ANIM_TITLE_LEN in boot_anim.h - only used for the
# soft landing-before-fade warning below, not emitted anywhere.
TITLE_LEN = 6


def fail(msg):
    sys.exit("gen_boot_anim_timeline.py: " + msg)


def warn(msg):
    print("gen_boot_anim_timeline.py: warning: " + msg, file=sys.stderr)


def meters_to_s3l(v):
    return round(v * S3L_F)


def degrees_to_s3l(v):
    return round(v / 360.0 * S3L_F)


def scale_to_s3l(v):
    return round(v * S3L_F)


# One space-unit is one meter - see boot_anim.h's own top comment - so this
# is the SAME conversion boot_anim.c's units() does for the curve/grid's own
# geometry, not small3dlib's fixed point (S3L_F above). BOOT_ANIM_ONE, not
# imported from boot_anim.h to keep this script standalone.
BOOT_ANIM_ONE = 4096


def meters_to_q12(v):
    return round(v * BOOT_ANIM_ONE)


def validate(cfg):
    timing = cfg["timing"]
    kfs = cfg["keyframes"]

    if len(kfs) < 2:
        fail("need at least two keyframes")

    if timing["grid_rings"] <= 0:
        fail("grid_rings must be positive (%r given)" % (timing["grid_rings"],))
    if cfg["grid_step_m"] <= 0:
        fail("grid_step_m must be positive (%r given)" % (cfg["grid_step_m"],))

    last_ms = -1
    for kf in kfs:
        if kf["ms"] <= last_ms:
            fail("keyframes must be strictly increasing in ms (%d after %d)"
                 % (kf["ms"], last_ms))
        last_ms = kf["ms"]
        if kf["ease"] not in EASE_NAMES:
            fail("unknown ease %r - must be one of %s" %
                 (kf["ease"], EASE_NAMES))
        for xf in TRANSFORMS:
            if xf not in kf:
                fail("keyframe at %d: missing %r transform" % (kf["ms"], xf))
            for axis in ("pos", "rot", "scale"):
                if len(kf[xf][axis]) != 3:
                    fail("keyframe at %d: %s.%s needs 3 components" %
                         (kf["ms"], xf, axis))

    pen_end = timing["pen_start_ms"] + timing["pen_ms"]
    if pen_end > timing["fade_start_ms"]:
        fail("pen_start_ms + pen_ms (%d) must not exceed fade_start_ms (%d) "
             "- the curve would still be drawing when the picture starts "
             "dissolving" % (pen_end, timing["fade_start_ms"]))

    if timing["fade_start_ms"] >= timing["total_ms"]:
        fail("fade_start_ms (%d) must be before total_ms (%d) - there would "
             "be no time left to dissolve" %
             (timing["fade_start_ms"], timing["total_ms"]))

    last_letter_lands = (timing["title_start_ms"] +
                         (TITLE_LEN - 1) * timing["title_stagger_ms"] +
                         timing["title_flight_ms"])
    if last_letter_lands > timing["fade_start_ms"]:
        warn("the last title letter lands at %d, after fade_start_ms (%d) - "
             "it will still be arriving when the picture starts dissolving"
             % (last_letter_lands, timing["fade_start_ms"]))

    if kfs[-1]["ms"] != timing["total_ms"]:
        warn("the last keyframe is at %d, not total_ms (%d) - the camera "
             "and space will hold their last keyframe values for the "
             "remainder" % (kfs[-1]["ms"], timing["total_ms"]))


TIMING_ORDER = [
    ("total_ms", "BOOT_ANIM_MS",
     "how long the whole animation runs, start to black"),
    ("axes_ms", "BOOT_ANIM_AXES_MS",
     "the axes grow out of the origin"),
    ("grid_start_ms", "BOOT_ANIM_GRID_START_MS", None),
    ("grid_ring_ms", "BOOT_ANIM_GRID_RING_MS",
     "each ring waits for the one inside"),
    ("grid_rings", "BOOT_ANIM_GRID_RINGS",
     "how many rings the floor draws before fading out"),
    ("grid_fade_ms", "BOOT_ANIM_GRID_FADE_MS", None),
    ("pen_start_ms", "BOOT_ANIM_PEN_START_MS", None),
    ("pen_ms", "BOOT_ANIM_PEN_MS", "how long the curve takes to draw"),
    ("title_start_ms", "BOOT_ANIM_TITLE_START_MS",
     "the title arrives after the curve, not during it"),
    ("title_stagger_ms", "BOOT_ANIM_TITLE_STAGGER_MS",
     "each letter starts this much after the one before it"),
    ("title_flight_ms", "BOOT_ANIM_TITLE_FLIGHT_MS",
     "how long ONE letter's flight is"),
    ("title_entry_px", "BOOT_ANIM_TITLE_ENTRY_PX",
     "how far off-panel a letter starts"),
    ("title_turns_phase", "BOOT_ANIM_TITLE_TURNS_PHASE",
     "wobble oscillations packed into the early part of the flight"),
    ("title_amplitude_px", "BOOT_ANIM_TITLE_AMPLITUDE_PX",
     "peak wobble swing right at the start"),
    ("title_wave_amplitude_px", "BOOT_ANIM_TITLE_WAVE_AMPLITUDE_PX",
     "the small idle wave once a letter has landed"),
    ("title_wave_period_ms", "BOOT_ANIM_TITLE_WAVE_PERIOD_MS", None),
    ("title_wave_stagger_ms", "BOOT_ANIM_TITLE_WAVE_STAGGER_MS", None),
    ("fade_start_ms", "BOOT_ANIM_FADE_START_MS",
     "dissolve into the launcher"),
    ("grid_hue_ms", "BOOT_ANIM_GRID_HUE_MS",
     "one whole hue-wheel turn takes this long"),
    ("grid_hue_spread", "BOOT_ANIM_GRID_HUE_SPREAD",
     "wheel positions per ring outward"),
    ("grid_whiten_max", "BOOT_ANIM_GRID_WHITEN_MAX", None),
    ("grid_ceiling_max", "BOOT_ANIM_GRID_CEILING_MAX", None),
    ("grid_max", "BOOT_ANIM_GRID_MAX", None),
]

EASE_ENUM = {"linear": "BOOT_ANIM_EASE_LINEAR",
             "ease_out": "BOOT_ANIM_EASE_OUT",
             "ease_in": "BOOT_ANIM_EASE_IN"}


def s3l_transform(xf):
    """A keyframe's one transform (plain meters/degrees/multiplier), converted
    to the six int32s (pos then rot; scale is separate - see caller) the
    generated struct actually stores."""
    pos = [meters_to_s3l(v) for v in xf["pos"]]
    rot = [degrees_to_s3l(v) for v in xf["rot"]]
    scale = [scale_to_s3l(v) for v in xf["scale"]]
    return pos, rot, scale


def main():
    if len(sys.argv) != 2:
        fail("usage: gen_boot_anim_timeline.py <boot_anim_timeline.json>")

    with open(sys.argv[1], "r", encoding="utf-8") as f:
        cfg = json.load(f)

    validate(cfg)

    # See gen_zeta_curve.py's own comment on this - LF only, so regenerating
    # on Windows does not rewrite the file for everyone else.
    sys.stdout.reconfigure(newline="\n")

    timing = cfg["timing"]
    kfs = cfg["keyframes"]

    w = sys.stdout.write
    w("/*=============================================================================\n")
    w(" * GENERATED FILE - do not edit.\n")
    w(" *\n")
    w(" *     python tools/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json > main/boot/boot_anim_timeline.h\n")
    w(" *\n")
    w(" * The boot animation's timing constants and its two keyframed 3D\n")
    w(" * transforms (camera, and the space the grid+curve live in), edited as\n")
    w(" * main/boot/boot_anim_timeline.json - by hand, or via\n")
    w(" * tools/boot_anim_editor.html's Bake button - and turned into this header by\n")
    w(" * this script. See that script's own top comment for what a keyframe is\n")
    w(" * (plain meters/degrees/multiplier units, converted to small3dlib's fixed\n")
    w(" * point right here) and boot_anim.h's boot_anim_timeline_sample() for how\n")
    w(" * they are interpolated.\n")
    w(" *===========================================================================*/\n")
    w("#pragma once\n\n#include <stdint.h>\n\n")

    for key, name, note in TIMING_ORDER:
        if note:
            w("/* %s */\n" % note)
        w("#define %s %d\n" % (name, timing[key]))
        w("\n" if note else "")

    w("/* small3dlib's S3L_Camera.focalLength - 0 is an orthographic\n")
    w(" * projection (see boot_anim.h's \"The projection\" section), any other\n")
    w(" * value a perspective one; S3L_F (512) is small3dlib's own \"normal\"\n")
    w(" * lens default. Authored directly in this unit - it is a lens\n")
    w(" * property, not a position or angle, so meters/degrees do not apply. */\n")
    w("#define BOOT_ANIM_CAMERA_FOCAL %d\n\n" % cfg["camera_focal"])

    w("/* The floor's ring spacing - see BOOT_ANIM_GRID_RINGS's own comment\n")
    w(" * in boot_anim.h. Authored in meters (grid_step_m in the JSON), like\n")
    w(" * a transform's own pos, and converted here the same way units() in\n")
    w(" * boot_anim.c does. */\n")
    w("#define BOOT_ANIM_GRID_STEP_Q12 %d\n\n" % meters_to_q12(cfg["grid_step_m"]))

    w("typedef enum {\n")
    w("    BOOT_ANIM_EASE_LINEAR = 0,   /* no easing - a plain ramp        */\n")
    w("    BOOT_ANIM_EASE_OUT    = 1,   /* tween_ease_out() - fast then settle */\n")
    w("    BOOT_ANIM_EASE_IN     = 2,   /* tween_ease_in() - slow then rush    */\n")
    w("} boot_anim_ease_t;\n\n")

    w("/* Both transforms' pos/rot/scale are small3dlib fixed point (S3L_F =\n")
    w(" * 512 = 1.0) already - converted from the JSON's plain meters/degrees/\n")
    w(" * multiplier units by this script, not at runtime. `ease` says how the\n")
    w(" * segment ENDING at this keyframe - from the previous one - is eased;\n")
    w(" * the first keyframe's is unused. */\n")
    w("typedef struct {\n")
    w("    uint32_t ms;\n")
    w("    int32_t  camera_pos[3];\n")
    w("    int32_t  camera_rot[3];\n")
    w("    int32_t  camera_scale[3];\n")
    w("    int32_t  space_pos[3];\n")
    w("    int32_t  space_rot[3];\n")
    w("    int32_t  space_scale[3];\n")
    w("    uint8_t  ease;\n")
    w("} boot_anim_keyframe_t;\n\n")

    w("#define BOOT_ANIM_KEYFRAME_COUNT %d\n\n" % len(kfs))
    w("static const boot_anim_keyframe_t boot_anim_keyframes[BOOT_ANIM_KEYFRAME_COUNT] = {\n")
    for kf in kfs:
        cam_pos, cam_rot, cam_scale = s3l_transform(kf["camera"])
        sp_pos, sp_rot, sp_scale = s3l_transform(kf["space"])
        w("    { %5d,\n" % kf["ms"])
        w("      { %6d, %6d, %6d }, { %6d, %6d, %6d }, { %6d, %6d, %6d },\n" % (
            cam_pos[0], cam_pos[1], cam_pos[2],
            cam_rot[0], cam_rot[1], cam_rot[2],
            cam_scale[0], cam_scale[1], cam_scale[2]))
        w("      { %6d, %6d, %6d }, { %6d, %6d, %6d }, { %6d, %6d, %6d },\n" % (
            sp_pos[0], sp_pos[1], sp_pos[2],
            sp_rot[0], sp_rot[1], sp_rot[2],
            sp_scale[0], sp_scale[1], sp_scale[2]))
        w("      %s },\n" % EASE_ENUM[kf["ease"]])
    w("};\n")


if __name__ == "__main__":
    main()
