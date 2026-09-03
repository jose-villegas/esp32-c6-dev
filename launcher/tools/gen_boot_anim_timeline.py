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

`camera_focal`, `grid_step_m` and `wave_height_m`/`wave_wavelength_m`/
`wave_period_ms` are not per-keyframe, unlike the transforms above:
`camera_focal` is a single lens setting (small3dlib's own S3L_Camera.
focalLength - see boot_anim.h's "The projection" section for what 0 does to
it: an orthographic projection, not a second code path to maintain);
`grid_step_m` is the spacing between floor rings, authored in meters like a
transform's `pos` and converted the same way; `wave_height_m`/
`wave_wavelength_m`/`wave_period_ms` are the ripple's own peak amplitude,
its crest-to-crest distance, and how long one full cycle takes to pass a
fixed point (see boot_anim.h's "The wave" section - a genuine radial sine,
height(r, t) = amplitude * sin(2*pi*r/wavelength - 2*pi*t/period), not
anchored to any one moment the way the front-based version this replaced
needed a start/end window for). `grid_rings` (how many rings the floor
draws before fading out) lives in `timing` instead, a plain count with
nothing to convert.

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


# Mirrors BOOT_ANIM_AXIS_FAR_UNITS/BOOT_ANIM_GRID_SPOKE_FAR_UNITS in
# boot_anim.h (both 500, by design - see that constant's own comment) -
# duplicated here rather than imported, same reason BOOT_ANIM_ONE above is:
# this script stays standalone. The worst-case LOCAL-SPACE coordinate an
# authored keyframe's own space/camera transform ever has to multiply -
# an axis/spoke tail is the longest reach this project draws, and this is
# its value BEFORE that transform (S3L_vec3Xmat4's own input), not after.
_FAR_UNITS = 500
_WORST_CASE_S3L_COORD = (_FAR_UNITS * BOOT_ANIM_ONE) >> 3  # BOOT_ANIM_ZETA_TO_S3L

# The scale-overflow guard below trades exactness for a real, defensible
# margin - see its own comment at the call site for the full derivation.
# Conservative on purpose: small3dlib's own matrix COMPOSITION step
# (S3L_mat4Xmat4 in small3dlib.h, also plain int32_t) has its own overflow
# risk that scales with rotation too, not just this scale product, and is
# not modeled exactly here - this check catches the dominant, easily-
# reasoned-about term (the final per-point multiply), not every path to
# the same failure.
_MAX_COMBINED_SCALE = 8


def check_transform_scale_overflow(kf):
    """small3dlib's own S3L_vec3Xmat4() (vendored, plain int32_t - this
    project does not build with S3L_USE_WIDER_TYPES) multiplies a camera-
    space coordinate by a composed space*camera matrix element that is
    itself proportional to authored scale*S3L_F. At this project's own
    largest authored reach (_WORST_CASE_S3L_COORD, from the 500-unit axis/
    spoke tail), INT32_MAX / (_WORST_CASE_S3L_COORD * S3L_F) is about
    16.4 - a combined space*camera scale anywhere near that silently wraps
    the point somewhere nonsensical (signed overflow is undefined
    behaviour in C, not guaranteed wraparound, so "wraps" is the observed
    behaviour, not a guarantee). _MAX_COMBINED_SCALE leaves real headroom
    under that, rather than cutting it close against an estimate that does
    not model every step of the composition exactly (see this file's own
    comment above)."""
    space_scale = max(abs(v) for v in kf["space"]["scale"])
    camera_scale = max(abs(v) for v in kf["camera"]["scale"])
    combined = space_scale * camera_scale
    if combined > _MAX_COMBINED_SCALE:
        fail("keyframe at %d: space.scale (max %r) * camera.scale (max %r) "
             "= %r, over this project's own %r safety margin against "
             "small3dlib's plain-int32_t point/matrix math silently "
             "overflowing at this project's largest authored reach "
             "(BOOT_ANIM_AXIS_FAR_UNITS/BOOT_ANIM_GRID_SPOKE_FAR_UNITS, "
             "both 500) - see boot_anim.h's own comment on "
             "BOOT_ANIM_AXIS_FAR_UNITS for the full mechanism" %
             (kf["ms"], space_scale, camera_scale, combined,
              _MAX_COMBINED_SCALE))


def validate(cfg):
    timing = cfg["timing"]
    kfs = cfg["keyframes"]

    if len(kfs) < 2:
        fail("need at least two keyframes")

    if timing["grid_rings"] <= 0:
        fail("grid_rings must be positive (%r given)" % (timing["grid_rings"],))
    if cfg["grid_step_m"] <= 0:
        fail("grid_step_m must be positive (%r given)" % (cfg["grid_step_m"],))
    if timing["grid_hue_ms"] <= 0:
        fail("grid_hue_ms must be positive (%r given) - boot_anim_grid_hue() "
             "divides by it every frame" % (timing["grid_hue_ms"],))
    if timing["title_wave_period_ms"] <= 0:
        fail("title_wave_period_ms must be positive (%r given) - "
             "boot_anim_title_wave() divides by it every frame" %
             (timing["title_wave_period_ms"],))
    if timing["grid_start_ms"] > timing["total_ms"]:
        fail("grid_start_ms (%d) must not be after total_ms (%d) - "
             "boot_anim_grid_climb() subtracts them as unsigned, so the "
             "grid would never climb at all" %
             (timing["grid_start_ms"], timing["total_ms"]))
    if timing["grid_spokes"] < 0:
        fail("grid_spokes must not be negative (%r given) - 0 hides them, "
             "there is no such thing as fewer" % (timing["grid_spokes"],))
    if timing["grid_spoke_dash"] not in (0, 1):
        fail("grid_spoke_dash must be 0 or 1 (%r given)" %
             (timing["grid_spoke_dash"],))
    if not 0 <= timing["title_shadow_alpha"] <= 255:
        fail("title_shadow_alpha must be 0..255 (%r given) - it is written "
             "straight into a #define read as a C uint8_t; a value outside "
             "that range would silently wrap rather than fail loudly on "
             "the device" % (timing["title_shadow_alpha"],))
    # 368/8/5 mirror BOOT_ANIM_TITLE_VIEW_H/the glyph cell in boot_anim.h -
    # the same "named here, not pulled from the C header" convention
    # suite_boot_anim.c's own top comment already uses for this panel's
    # fixed dimensions. Only a COARSE bound: title_amplitude_px/
    # title_wave_amplitude_px are the two knobs that can carry a landed
    # letter off title_height_px, so they stand in as a margin rather than
    # replaying tween_ease_out()'s own curve here - the exact simulation
    # is test_the_title_stays_on_the_panel_once_visible() in
    # suite_boot_anim.c, which this only backstops for a value obviously
    # wrong enough that no test run would ever be needed to see it.
    _title_view_h = 368
    _title_cell_h = 8 * 5
    _title_margin = (timing["title_amplitude_px"] +
                     timing["title_wave_amplitude_px"])
    if (timing["title_height_px"] - _title_margin < 0 or
            timing["title_height_px"] + _title_cell_h + _title_margin >
            _title_view_h):
        fail("title_height_px=%r would carry the title off the top or "
             "bottom of the %d-pixel-tall viewer's frame once its own "
             "wobble (title_amplitude_px=%r) and idle wave "
             "(title_wave_amplitude_px=%r) are accounted for" %
             (timing["title_height_px"], _title_view_h,
              timing["title_amplitude_px"], timing["title_wave_amplitude_px"]))
    if timing["image_start_ms"] < 0 or timing["image_fade_ms"] < 0:
        fail("image_start_ms/image_fade_ms must not be negative (%r/%r "
             "given) - boot_anim_image_reveal() hands both to tween_ramp() "
             "as a uint32_t, where a negative duration wraps to an "
             "enormous one and the crossfade would silently never finish" %
             (timing["image_start_ms"], timing["image_fade_ms"]))

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
        check_transform_scale_overflow(kf)

    pen_end = timing["pen_start_ms"] + timing["pen_ms"]
    if pen_end > timing["fade_start_ms"]:
        fail("pen_start_ms + pen_ms (%d) must not exceed fade_start_ms (%d) "
             "- the curve would still be drawing when the picture starts "
             "dissolving" % (pen_end, timing["fade_start_ms"]))

    if timing["pen_finish_ms"] <= pen_end:
        fail("pen_finish_ms (%d) must be after pen_start_ms + pen_ms (%d) - "
             "phase 2 needs positive duration to actually finish drawing "
             "the curve in" % (timing["pen_finish_ms"], pen_end))

    if timing["fade_start_ms"] >= timing["total_ms"]:
        fail("fade_start_ms (%d) must be before total_ms (%d) - there would "
             "be no time left to dissolve" %
             (timing["fade_start_ms"], timing["total_ms"]))

    if cfg["wave_height_m"] != 0 and cfg["wave_wavelength_m"] <= 0:
        warn("wave_wavelength_m (%r) is not positive, so the ripple is "
             "silently invisible despite wave_height_m (%r) being nonzero"
             % (cfg["wave_wavelength_m"], cfg["wave_height_m"]))
    if (cfg["wave_height_m"] != 0 and
            timing["wave_out_ms"] > timing["total_ms"]):
        warn("wave_out_ms (%d) is after total_ms (%d) - the ripple will "
             "still be at full strength when the picture ends, never "
             "having started to fade out"
             % (timing["wave_out_ms"], timing["total_ms"]))
    if (cfg["wave_height_m"] != 0 and
            timing["wave_in_ms"] > timing["wave_out_ms"]):
        warn("wave_in_ms (%d) is after wave_out_ms (%d) - the ripple will "
             "still be arriving while it is already meant to be leaving, "
             "with no plateau at full strength in between"
             % (timing["wave_in_ms"], timing["wave_out_ms"]))

    last_letter_lands = (timing["title_start_ms"] +
                         (TITLE_LEN - 1) * timing["title_stagger_ms"] +
                         timing["title_flight_ms"])
    if last_letter_lands > timing["fade_start_ms"]:
        warn("the last title letter lands at %d, after fade_start_ms (%d) - "
             "it will still be arriving when the picture starts dissolving"
             % (last_letter_lands, timing["fade_start_ms"]))

    # Only when the crossfade is actually authored to happen at all - the
    # same guard the wave's own warns above use (cfg["wave_height_m"] !=
    # 0), and for the same reason: the inert backward-compat default
    # (image_start_ms == total_ms, see its own setdefault comment) would
    # otherwise trip this on every regenerate of a file with no
    # photograph in it.
    if timing["image_start_ms"] < timing["total_ms"]:
        image_done = timing["image_start_ms"] + timing["image_fade_ms"]
        if image_done > timing["fade_start_ms"]:
            warn("the photograph finishes crossing in at %d, after "
                 "fade_start_ms (%d) - it will still be arriving as the "
                 "picture starts dissolving"
                 % (image_done, timing["fade_start_ms"]))

    if timing["pen_finish_ms"] > timing["fade_start_ms"]:
        warn("pen_finish_ms (%d) is after fade_start_ms (%d) - the curve "
             "will still be drawing when the picture starts dissolving"
             % (timing["pen_finish_ms"], timing["fade_start_ms"]))

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
    ("grid_spokes", "BOOT_ANIM_GRID_SPOKES",
     "radial guide lines from the origin to the floor's own edge - 0 hides them"),
    ("grid_spoke_dash", "BOOT_ANIM_GRID_SPOKE_DASH",
     "draws each spoke as alternating dashes instead of a solid line - "
     "0 solid, 1 dashed"),
    ("grid_spoke_start_ms", "BOOT_ANIM_GRID_SPOKE_START_MS",
     "spokes start drawing outward from the origin"),
    ("grid_spoke_draw_ms", "BOOT_ANIM_GRID_SPOKE_DRAW_MS",
     "how long a spoke takes to reach the floor's own edge - 0 draws the "
     "full length instantly"),
    ("grid_fade_ms", "BOOT_ANIM_GRID_FADE_MS", None),
    ("wave_in_ms", "BOOT_ANIM_WAVE_IN_MS",
     "the ripple is muted before this, then lerps up to full strength"),
    ("wave_out_ms", "BOOT_ANIM_WAVE_OUT_MS",
     "the ripple is at full strength until this, then lerps back to muted"),
    ("pen_start_ms", "BOOT_ANIM_PEN_START_MS", None),
    ("pen_ms", "BOOT_ANIM_PEN_MS", "how long the curve takes to draw"),
    ("pen_finish_ms", "BOOT_ANIM_PEN_FINISH_MS",
     "when the curve reaches the table's true end - independent of "
     "fade_start_ms, so it can keep drawing well past phase 1"),
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
    ("title_height_px", "BOOT_ANIM_TITLE_VIEW_Y",
     "how far down the viewer's frame the title's own centre lands - "
     "see boot_anim.h's own comment on this section for the frame it is in"),
    ("title_shadow_dx", "BOOT_ANIM_TITLE_SHADOW_DX",
     "drop shadow offset, pixels right (negative is left) - 0/0 disables it"),
    ("title_shadow_dy", "BOOT_ANIM_TITLE_SHADOW_DY",
     "drop shadow offset, pixels down (negative is up)"),
    ("title_shadow_alpha", "BOOT_ANIM_TITLE_SHADOW_ALPHA",
     "shadow density, 0..255 - 0 invisible, 255 solid, between is a "
     "dithered fake transparency (see gfx_fill_rect_dither() in gfx.c)"),
    ("image_start_ms", "BOOT_ANIM_IMAGE_START_MS",
     "the photograph starts crossing in as the function crosses out"),
    ("image_fade_ms", "BOOT_ANIM_IMAGE_FADE_MS",
     "how long that one shared crossfade takes - 0 cuts straight to the photo"),
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

    # pen_finish_ms is newer than fade_start_ms - a file baked before it
    # existed has no way to carry it. Defaulting it to fade_start_ms
    # reproduces exactly what boot_anim_pen() always did before the two
    # were split apart (see its own "TWO PHASES" comment), so an old
    # timeline keeps behaving the way it always did rather than failing to
    # load at all over a field its author never had reason to set.
    timing = cfg.get("timing", {})
    timing.setdefault("pen_finish_ms", timing.get("fade_start_ms"))

    # The wave is newer still - a file baked before it existed has no
    # wave_* fields at all. Defaulting the height to 0 turns it off
    # outright regardless of wavelength/period, the same "behaves exactly
    # like before this existed" reasoning pen_finish_ms's own default above
    # uses - an old timeline should not suddenly grow a ripple its author
    # never asked for.
    cfg.setdefault("wave_height_m", 0)
    # wave_wavelength_m/wave_period_ms replace an EARLIER version's
    # wave_decay_m/wave_start_ms/wave_end_ms/wave_ease outright - a genuine
    # radial sine now, not a travelling front with a decaying trail behind
    # it (see boot_anim.h's own comment on why) - so this is not a faithful
    # reproduction of the old shape for anyone who already had a nonzero
    # wave_height_m under that model, the same honest caveat the
    # front-based rewrite before THIS one already carried (the two are not
    # the same picture). Three ring-spacings and three seconds are simply
    # reasonable starting points to look at through the editor, not a
    # migration.
    cfg.setdefault("wave_wavelength_m", 3 * cfg.get("grid_step_m", 1))
    cfg.setdefault("wave_period_ms", 3000)
    # wave_in_ms/wave_out_ms are newer again - starting to lerp in a
    # second in, and starting to lerp back out a second before the end,
    # are simply reasonable starting points, the same "look at it through
    # the editor" reasoning wave_wavelength_m/wave_period_ms's own
    # defaults above use.
    timing.setdefault("wave_in_ms", 1000)
    timing.setdefault("wave_out_ms",
                      max(1000, timing.get("total_ms", 5800) - 1000))
    # grid_spokes is newer than the polar grid itself - the radial guide
    # lines used to be a fixed 8, unauthored; 8 is the exact same default
    # for a file baked before this existed, so it keeps looking the way it
    # always did.
    timing.setdefault("grid_spokes", 8)
    # grid_spoke_dash is newer still - a file baked before it existed drew
    # every spoke solid, so 0 (solid) is what keeps it looking the same.
    timing.setdefault("grid_spoke_dash", 0)
    # grid_spoke_start_ms/grid_spoke_draw_ms are newer again - a file baked
    # before they existed drew every spoke at full length the instant it
    # was eligible to show at all, with no outward reveal of its own. 0/0
    # reproduces exactly that: tween_ramp()'s own "dur_ms of 0 jumps
    # straight to 255 the instant now_ms passes start_ms" (see tween.h)
    # means a spoke is already full length by the first frame it would
    # have appeared in before these existed.
    timing.setdefault("grid_spoke_start_ms", 0)
    timing.setdefault("grid_spoke_draw_ms", 0)
    # image_start_ms/image_fade_ms are newer again - a file baked before
    # they existed had no photograph in it at all, and total_ms is what
    # reproduces that exactly: boot_anim_run() never draws a frame past
    # total_ms, so defaulting image_start_ms TO total_ms means boot_anim_
    # image_reveal() is 0 for every frame that is ever actually drawn -
    # the crossfade is authored but never reached, not merely set to a
    # short/instant duration, until someone deliberately pulls it
    # earlier. image_fade_ms's own default only matters once that happens.
    # title_height_px is newer than the title itself - BOOT_ANIM_TITLE_
    # VIEW_Y used to be a fixed constant in boot_anim.h (derived from a
    # golden-rectangle construction - see that section's own comment,
    # still there as this default's own reasoning), so 50 - what it was
    # fixed at - is what keeps a file baked before this existed landing
    # the title exactly where it always did.
    timing.setdefault("title_height_px", 50)
    # title_shadow_dx/dy are newer again - the shadow used to be a fixed
    # (1, 1) two gfx_text_font() calls always drew, so 1/1 is what keeps
    # a file baked before this existed looking exactly the same rather
    # than silently losing its shadow.
    timing.setdefault("title_shadow_dx", 1)
    timing.setdefault("title_shadow_dy", 1)
    # title_shadow_alpha is newer again - the shadow used to be fully
    # solid (there was no dithering to be less than that), so 255 is
    # what keeps a file baked before this existed looking exactly the
    # same rather than silently turning translucent.
    timing.setdefault("title_shadow_alpha", 255)
    timing.setdefault("image_start_ms", timing.get("total_ms", 5800))
    timing.setdefault("image_fade_ms", 800)

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

    w("/* The wave's own peak amplitude - see boot_anim.h's \"The wave\"\n")
    w(" * section. Authored in meters (wave_height_m in the JSON), the same\n")
    w(" * as grid_step_m just above; 0 (the default for a file baked before\n")
    w(" * this existed - see this script's own backward-compatibility\n")
    w(" * comment) turns the ripple off outright, not just down. */\n")
    w("#define BOOT_ANIM_WAVE_HEIGHT_Q12 %d\n\n" % meters_to_q12(cfg["wave_height_m"]))

    w("/* The wave's own crest-to-crest distance - see boot_anim.h's \"The\n")
    w(" * wave\" section. Also meters, also authored (wave_wavelength_m in\n")
    w(" * the JSON). */\n")
    w("#define BOOT_ANIM_WAVE_WAVELENGTH_Q12 %d\n\n" %
      meters_to_q12(cfg["wave_wavelength_m"]))

    w("/* How long one full cycle takes to pass a fixed point - milliseconds,\n")
    w(" * authored (wave_period_ms in the JSON), the same \"how long one\n")
    w(" * cycle takes\" unit title_wave_period_ms already is for the title's\n")
    w(" * own wobble. */\n")
    w("#define BOOT_ANIM_WAVE_PERIOD_MS %d\n\n" % cfg["wave_period_ms"])

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
