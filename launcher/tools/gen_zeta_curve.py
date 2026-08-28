#!/usr/bin/env python3
"""Generate main/boot_anim_curve.h - the zeta curve the startup animation draws.

    python tools/gen_zeta_curve.py > main/boot_anim_curve.h

WHY THIS IS A TABLE AND NOT DEVICE CODE

The animation plots zeta(1/2 + it) as t rises. That value cannot be had
cheaply on an ESP32-C6: the Dirichlet series does not converge on the
critical line, the alternating series that does converges far too slowly to
be worth 240 evaluations a frame, and the chip is RV32IMAC - no FPU, so
every step of any of it is a soft-float library call.

None of that matters, because the curve never changes. It is one fixed
polyline, so it is computed here, once, in double precision, and shipped in
.rodata where it is memory-mapped from flash at no cost in RAM - about 1.4
KiB against a 16 MB part with 87% of the app partition free.

HOW ZETA IS EVALUATED

Borwein's acceleration of the alternating (Dirichlet eta) series, which
gives full double precision with nothing but the standard library:

    eta(s) ~ -1/d[n] * sum over k of (-1)^k (d[k] - d[n]) / (k+1)^s
    zeta(s) = eta(s) / (1 - 2^(1-s))

Checked against known values before anything is emitted - see main(). If
those checks ever fail this exits rather than printing a plausible-looking
table of wrong numbers.

HOW THE CURVE IS SAMPLED

At equal steps of DISTANCE ON SCREEN, not of t. Sampling uniformly in t
would crowd points into the slow stretches near the axis and stretch them
thin across the fast outer loops, which is where the polyline would show.
The screen metric mixes the two axes at the ratio the projection uses them
(T_PER_Z below, which must match boot_anim.h's), so one table entry is one
roughly constant number of pixels wherever it lands.

That is also what lets the pen move at a steady speed by simply walking the
table one entry at a time: index IS arc length, near enough.
"""

import math
import sys
from math import factorial

# --- zeta -----------------------------------------------------------------

# 100, not the 64 that was enough while the climb stopped at t ~ 70: Borwein's
# error bound degrades with the imaginary part, and extending the climb to
# t ~ 126 brought the same failure back at the new top - an order of 80 was
# only good to ~3e-4 by t ~ 124 - which the zero check below caught again,
# being an assertion about a number that should be zero rather than about a
# number that merely looks plausible. 100 is good to ~3e-9 at the new
# farthest zero checked (t ~ 124.3), comfortably past this file's 1e-6 bar.
_N = 100


def _borwein_d(n):
    d = []
    for k in range(n + 1):
        s = 0.0
        for i in range(k + 1):
            s += factorial(n + i - 1) * 4 ** i / (factorial(n - i) * factorial(2 * i))
        d.append(n * s)
    return d


_D = _borwein_d(_N)


def eta(s):
    total = 0
    for k in range(_N):
        total += (-1) ** k * (_D[k] - _D[_N]) / (k + 1) ** s
    return -total / _D[_N]


def zeta(s):
    return eta(s) / (1 - 2 ** (1 - s))


# --- what gets drawn ------------------------------------------------------

# These four MUST match boot_anim.h. They are here because the sampling
# depends on them: a different height or scale wants its points spaced
# differently.
T_MAX = 126.0       # BOOT_ANIM_T_MAX - int16 Q8 tops out at 32767/256 =
                    # 127.996; stopped short of that rather than at it, so
                    # rounding a sample's own t up never overflows
T_PER_Z = 9.0 / 35.0  # BOOT_ANIM_T_PX / BOOT_ANIM_Z_PX - unchanged: a
                      # property of the projection, not of how far up it climbs
STEP = 0.14         # target spacing between samples, in plane units - was
                    # 0.18; tightened so the table stays at least 3x its
                    # previous length even though most of the extra T_MAX
                    # this climb already had (70) went into this step
                    # rather than height alone
Q = 12              # BOOT_ANIM_Q, for re/im
TQ = 8              # t is stored in Q8: 126 * 256 still fits an int16

# Where phase 1 of the reveal - the part boot_anim.h's boot_anim_pen() paces
# identically to before this climb was extended - hands off to phase 2. The
# original T_MAX: everything up to here is (up to floating-point noise in
# the arc-length walk below) the same table this file shipped when the climb
# stopped at 35, so the picture during phase 1 does not change. See
# boot_anim_pen()'s own comment for what happens at this handoff.
PHASE1_T_MAX = 35.0

# The imaginary parts of the first nontrivial zeros of zeta, from the
# literature (Odlyzko's tables). The curve is drawn crossing the t axis at
# each of these, which is a claim the generated table has to bear out - see
# the assertion below, and suite_boot_anim.c, which re-checks it on the
# rounded integers that actually ship.
ZEROS = [
    14.134725142, 21.022039639, 25.010857580, 30.424876126, 32.935061588,
    37.586178159, 40.918719012, 43.327073281, 48.005150881, 49.773832478,
    52.970321478, 56.446247697, 59.347044003, 60.831778525, 65.112544048,
    67.079810529, 69.546401711, 72.067157674, 75.704690699, 77.144840069,
    79.337375020, 82.910380854, 84.735492981, 87.425274613, 88.809111208,
    92.491899271, 94.651344041, 95.870634228, 98.831194218, 101.317851006,
    103.725538040, 105.446623052, 107.168611184, 111.029535543, 111.874659177,
    114.320220915, 116.226680321, 118.790782866, 121.370125002, 122.946829294,
    124.256818554,
]


def sample():
    """The curve, at roughly equal spacing on screen.

    Returns (points, phase1_count): phase1_count is how many of the leading
    points fall at or before PHASE1_T_MAX - see that constant's own comment.
    """
    # fine/(fine-1) scaled up with T_MAX from the 8000/35 this was tuned at,
    # so the spacing of the underlying grid - and so which points the
    # arc-length walk below picks - is the same over [0, PHASE1_T_MAX] as it
    # always was, not coarsened by covering twice the range with the same
    # number of samples.
    fine = round(8000 * T_MAX / 35.0)
    raw = [(T_MAX * i / (fine - 1), zeta(complex(0.5, T_MAX * i / (fine - 1))))
           for i in range(fine)]

    out = [raw[0]]
    phase1_count = 1
    acc = 0.0
    for i in range(1, fine):
        dt = raw[i][0] - raw[i - 1][0]
        dz = raw[i][1] - raw[i - 1][1]
        acc += math.hypot(abs(dz), dt * T_PER_Z)
        if acc >= STEP:
            out.append(raw[i])
            acc = 0.0
            if raw[i][0] <= PHASE1_T_MAX:
                phase1_count = len(out)
    if out[-1][0] != raw[-1][0]:
        out.append(raw[-1])
    return out, phase1_count


def q(value, bits):
    return int(round(value * (1 << bits)))


def main():
    # Windows would otherwise turn every \n on stdout into \r\n, so
    # regenerating on one machine would rewrite every line of the file for
    # anyone on another. This header sits beside .c files stored with LF.
    sys.stdout.reconfigure(newline="\n")

    # Refuse to emit anything unless the evaluator is demonstrably right.
    for s, want in ((complex(2, 0), 1.6449340668482264),
                    (complex(0.5, 0), -1.4603545088095868)):
        if abs(zeta(s) - want) > 1e-9:
            sys.exit(f"zeta({s}) = {zeta(s)}, expected {want}")
    for t in ZEROS:
        if abs(zeta(complex(0.5, t))) > 1e-6:
            sys.exit(f"zeta(1/2 + {t}i) should vanish, got {zeta(complex(0.5, t))}")

    pts, phase1_count = sample()
    for _, z in pts:
        if abs(q(z.real, Q)) > 32767 or abs(q(z.imag, Q)) > 32767:
            sys.exit("a sample does not fit in an int16 at this Q")

    w = sys.stdout.write
    w("/*=============================================================================\n")
    w(" * GENERATED FILE - do not edit.\n")
    w(" *\n")
    w(" *     python tools/gen_zeta_curve.py > main/boot_anim_curve.h\n")
    w(" *\n")
    w(" * zeta(1/2 + it) for t from 0 to %g, sampled at roughly equal spacing on\n" % T_MAX)
    w(" * screen. See tools/gen_zeta_curve.py for how the values are computed and\n")
    w(" * why they are a table rather than device code, and boot_anim.h for what is\n")
    w(" * done with them.\n")
    w(" *\n")
    w(" * re and im are Q%d (%d is 1.0); t is Q%d (%d is 1.0).\n" % (Q, 1 << Q, TQ, 1 << TQ))
    w(" *===========================================================================*/\n")
    w("#pragma once\n\n#include <stdint.h>\n\n")

    w("#define BOOT_ANIM_CURVE_POINTS %d\n\n" % len(pts))

    w("/* How many of the leading points are phase 1 of the reveal - see\n"
      " * PHASE1_T_MAX's own comment in tools/gen_zeta_curve.py and\n"
      " * boot_anim_pen()'s in boot_anim.h. */\n")
    w("#define BOOT_ANIM_CURVE_PHASE1_POINTS %d\n\n" % phase1_count)

    w("typedef struct {\n    int16_t re, im;   /* Q%d */\n    int16_t t;        /* Q%d */\n"
      "} boot_anim_sample_t;\n\n" % (Q, TQ))

    w("static const boot_anim_sample_t boot_anim_curve[BOOT_ANIM_CURVE_POINTS] = {\n")
    for t, z in pts:
        w("    { %6d, %6d, %5d },\n" % (q(z.real, Q), q(z.imag, Q), q(t, TQ)))
    w("};\n\n")

    w("/* Where the curve meets the t axis. Q%d, same as the samples' t.\n" % TQ)
    w(" *\n")
    w(" * Not decoration: these are the heights at which zeta vanishes, and the\n")
    w(" * curve passing through the axis exactly there is the one fact the picture\n")
    w(" * is actually asserting. suite_boot_anim.c checks the shipped table against\n")
    w(" * them. */\n")
    w("#define BOOT_ANIM_ZEROS %d\n\n" % len(ZEROS))
    w("static const int16_t boot_anim_zero_t[BOOT_ANIM_ZEROS] = {\n    ")
    w(", ".join(str(q(t, TQ)) for t in ZEROS))
    w(",\n};\n")


if __name__ == "__main__":
    main()
