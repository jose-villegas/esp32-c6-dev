#!/usr/bin/env python3
"""Generate main/boot/boot_anim_timeline.h - the boot animation's timing and
camera keyframes.

    python tools/gen_boot_anim_timeline.py main/boot/boot_anim_timeline.json > main/boot/boot_anim_timeline.h

WHERE THIS DATA COMES FROM

main/boot/boot_anim_timeline.json is the source of truth: hand-edited, or
edited through tools/boot_anim_editor.html's "Bake" button, which downloads a
JSON file in exactly this shape to replace it. This script never talks to
the editor directly - the two are joined only by the file format.

WHAT IS A KEYFRAME HERE

Position (the view origin, in panel pixels), rotation (the camera's pitch
phase - boot_anim_sin()'s 0..65536-per-turn convention; only the X component
is read by the current animation, Y/Z are carried for a future one that
might want them) and scale (one uniform "motif shrink" factor, again
mirrored into X/Y/Z for forward-compatibility though only ever one value
today). boot_anim.h's boot_anim_timeline_sample() walks this table and lerps
between the two keyframes bracketing `now_ms`, easing the fraction with
whichever of linear/ease_out/ease_in the arriving keyframe names - the same
three shapes util/tween.h already provides, so nothing new is needed on the
firmware side to interpret them.

The `timing` block is everything else that paces the animation but is not a
transform of the space: how fast the grid rings fade in, how the title
letters fly in and wobble, how the grid's own hue travels. Plain values,
straight through to #define.

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

# Must match BOOT_ANIM_TITLE_LEN in boot_anim.h - only used for the
# soft landing-before-fade warning below, not emitted anywhere.
TITLE_LEN = 6


def fail(msg):
    sys.exit("gen_boot_anim_timeline.py: " + msg)


def warn(msg):
    print("gen_boot_anim_timeline.py: warning: " + msg, file=sys.stderr)


def validate(cfg):
    timing = cfg["timing"]
    kfs = cfg["keyframes"]

    if len(kfs) < 2:
        fail("need at least two keyframes")

    last_ms = -1
    for kf in kfs:
        if kf["ms"] <= last_ms:
            fail("keyframes must be strictly increasing in ms (%d after %d)"
                 % (kf["ms"], last_ms))
        last_ms = kf["ms"]
        if kf["ease"] not in EASE_NAMES:
            fail("unknown ease %r - must be one of %s" %
                 (kf["ease"], EASE_NAMES))
        for axis in ("pos", "rot", "scale"):
            if len(kf[axis]) != (2 if axis == "pos" else 3):
                fail("keyframe at %d: %s needs %d components" %
                     (kf["ms"], axis, 2 if axis == "pos" else 3))
        for v in list(kf["pos"]) + list(kf["rot"]) + list(kf["scale"]):
            if abs(v) > 32767:
                fail("keyframe at %d: a component does not fit an int16" %
                     kf["ms"])

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
             "will hold its last keyframe value for the remainder" %
             (kfs[-1]["ms"], timing["total_ms"]))


TIMING_ORDER = [
    ("total_ms", "BOOT_ANIM_MS",
     "how long the whole animation runs, start to black"),
    ("axes_ms", "BOOT_ANIM_AXES_MS",
     "the axes grow out of the origin"),
    ("grid_start_ms", "BOOT_ANIM_GRID_START_MS", None),
    ("grid_ring_ms", "BOOT_ANIM_GRID_RING_MS",
     "each ring waits for the one inside"),
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
    w(" * The boot animation's timing constants and camera keyframes, edited as\n")
    w(" * main/boot/boot_anim_timeline.json - by hand, or via\n")
    w(" * tools/boot_anim_editor.html's Bake button - and turned into this header by\n")
    w(" * this script. See that script's own top comment for what a keyframe is and\n")
    w(" * boot_anim.h's boot_anim_timeline_sample() for how they are interpolated.\n")
    w(" *===========================================================================*/\n")
    w("#pragma once\n\n#include <stdint.h>\n\n")

    for key, name, note in TIMING_ORDER:
        if note:
            w("/* %s */\n" % note)
        w("#define %s %d\n" % (name, timing[key]))
        w("\n" if note else "")

    w("typedef enum {\n")
    w("    BOOT_ANIM_EASE_LINEAR = 0,   /* no easing - a plain ramp        */\n")
    w("    BOOT_ANIM_EASE_OUT    = 1,   /* tween_ease_out() - fast then settle */\n")
    w("    BOOT_ANIM_EASE_IN     = 2,   /* tween_ease_in() - slow then rush    */\n")
    w("} boot_anim_ease_t;\n\n")

    w("/* pos/rot are panel pixels and a boot_anim_sin() phase; scale is Q8\n")
    w(" * (256 == 1.0). `ease` says how the segment ENDING at this keyframe -\n")
    w(" * from the previous one - is eased; the first keyframe's is unused. Only\n")
    w(" * rot[0] and scale[0] are read by boot_anim.h today - see this file's own\n")
    w(" * top comment. */\n")
    w("typedef struct {\n")
    w("    uint32_t ms;\n")
    w("    int16_t  pos[2];\n")
    w("    int16_t  rot[3];\n")
    w("    int16_t  scale[3];\n")
    w("    uint8_t  ease;\n")
    w("} boot_anim_keyframe_t;\n\n")

    w("#define BOOT_ANIM_KEYFRAME_COUNT %d\n\n" % len(kfs))
    w("static const boot_anim_keyframe_t boot_anim_keyframes[BOOT_ANIM_KEYFRAME_COUNT] = {\n")
    for kf in kfs:
        w("    { %5d, { %4d, %4d }, { %6d, %4d, %4d }, { %4d, %4d, %4d }, %s },\n" % (
            kf["ms"], kf["pos"][0], kf["pos"][1],
            kf["rot"][0], kf["rot"][1], kf["rot"][2],
            kf["scale"][0], kf["scale"][1], kf["scale"][2],
            EASE_ENUM[kf["ease"]]))
    w("};\n")


if __name__ == "__main__":
    main()
