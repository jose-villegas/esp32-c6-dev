#!/usr/bin/env python3
"""compare_counters.py - two-ref counter bisection driver (bd esp32c6-8zx).

Attributes a device regression by COUNTING, not timing: host wall-clock
comparisons across two separately-linked binaries carry a 7-15% cross-binary
noise floor (see docs/Sand/Perf-Round-Guide.md, "Count, do not time"), which
swamped the very question this tool was built to answer. Work counters
(sand_work_counters.h) are exact and deterministic instead - two builds of
byte-identical source produce byte-identical counts - so a real change shows
up as a real difference and a no-op window shows up as EXACT equality
(bd esp32c6-8zx's own signature for "nothing happened here": twelve counters
landing on the same digit is not noise, it is proof the window was wrong).

WHAT THIS SCRIPT DOES

For each of two refs:
  1. `git archive <ref> -- launcher/main` into a fresh scratch directory -
     never `git checkout <ref>` in this worktree, which would fight every
     other session using it (Perf-Round-Guide.md's own house rule).
  2. Overlay the CURRENT tree's counters_scene_main.c and
     sand_work_counters.h/.c into that scratch tree - the "probe support
     files" that must be byte-identical on both sides, so the harness
     itself is never a variable between the two builds. Neither depends on
     anything that changes across the range this tool is meant to bisect:
     they call nothing but sand.h's own long-stable public API.
  3. Inject the SAME sand_work_counters.h include and SAND_WORK_COUNT() call
     sites into the scratch tree's OWN sand_liquid.c, at fixed text anchors -
     see inject_counters() below. This is the one genuinely fiddly part
     (rebuilt from scratch three times by three different agents before this
     landed - see the commit that added this file). The anchors are lines
     that predate the counters landing by a wide margin on this tree's own
     history; if one goes missing at some future ref, inject_counters()
     fails loudly rather than silently mismeasuring.
  4. Compile counters_scene_main.c against the ref's OWN algorithm sources
     (sand.c, sand_liquid.c now carrying the injected counters, sand_gas.c,
     sand_reactions.c, sand_plants.c when the ref has it, material.c,
     palette.c, row_runs.c, sand_ui.c, tilt.c)
     plus the carried-in sand_work_counters.c, and run it.

Then prints one table: per-counter values at each ref, the delta, and the
percentage change.

USAGE

    python compare_counters.py REF_A [REF_B] [--scene water] [--steps 20]

REF_B defaults to REF_A^ (its parent) - the common case is "did this one
commit change the work", which is exactly a bisect step.

Only the water scene is wired up today (counters_scene_main.c's own doc
comment says why); pass --scene to see the clear error rather than a silent
wrong answer if that ever needs to grow.
"""
import argparse
import shutil
import subprocess
import sys
import tarfile
import tempfile
import io
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP_SAND = HERE.parent.parent           # launcher/main/apps/sand
MAIN_DIR = APP_SAND.parent.parent       # launcher/main
REPO_ROOT = MAIN_DIR.parent.parent      # repo root


# One (anchor, appended-text) pair per counter site. Applied as
# text.replace(anchor, anchor + appended, 1) - `appended` always comes
# AFTER the anchor, so a mid-block anchor's own scope is never in doubt.
# Every anchor here is verified (see this file's own commit) to appear
# exactly once in sand_liquid.c at every ref this tool has actually been
# run against; a ref where one goes missing raises rather than guesses.
_INJECTIONS = [
    ('#include "sand_priv.h"\n',
     '#include "sand_work_counters.h"\n'),

    ('    bool found_any = false;\n',
     '\n    SAND_WORK_COUNT(xflow_calls);\n'),

    ('    for (int y = y_from; y != y_to; y += y_step) {\n',
     '        SAND_WORK_COUNT(rows_walked);\n'),

    ('    for (int bx = bx_from; bx != bx_to; bx += x_step) {\n',
     '        SAND_WORK_COUNT(blocks_considered);\n'),

    ('        if (brow != NULL && (brow[bx] & BLOCK_LIQUID_NEAR) == 0) {\n'
     '            continue;\n'
     '        }\n',
     '        SAND_WORK_COUNT(blocks_examined);\n'),

    ('    for (int x = cx_from; x != cx_to; x += x_step) {\n',
     '        SAND_WORK_COUNT(cells_examined);\n'),

    ('    const uint8_t id = CELL_MATERIAL(c);\n'
     '    if (((is_liquid >> id) & 1u) == 0) {\n'
     '        return false;\n'
     '    }\n',
     '    SAND_WORK_COUNT(cells_passing_mask);\n'),

    ('    if (has_room_below(s, x, y, dx, dy, id)) {\n',
     None),  # equalise_one_cell_calls goes BEFORE this anchor - see below

    ('    int lowest, at;\n',
     '    SAND_WORK_COUNT(find_shallowest_calls);\n'),

    ('    for (int k = 1; k <= sight; k++) {\n',
     '        SAND_WORK_COUNT(find_shallowest_iters);\n'),

    ('    const int tx = x + px * at;\n',
     None),  # transfers goes BEFORE this anchor - see below

    ('    int mass = CELL_VARIANT(grain);\n',
     None),  # move_liquid_grain_calls goes BEFORE this anchor - see below
]

# The three counters that have to land BEFORE their anchor line (the anchor
# is the first statement of the scope being counted, not the last statement
# of the block that precedes it): (anchor, text-to-insert-before-it).
_PREPEND_INJECTIONS = [
    ('    if (has_room_below(s, x, y, dx, dy, id)) {\n',
     '    SAND_WORK_COUNT(equalise_one_cell_calls);\n'),
    ('    const int tx = x + px * at;\n',
     '    SAND_WORK_COUNT(transfers);\n'),
    ('    int mass = CELL_VARIANT(grain);\n',
     '    SAND_WORK_COUNT(move_liquid_grain_calls);\n'),
]


def inject_counters(text: str) -> str:
    for anchor, appended in _INJECTIONS:
        if appended is None:
            continue
        n = text.count(anchor)
        if n != 1:
            raise RuntimeError(
                f"inject_counters: anchor appears {n} times, expected 1: "
                f"{anchor!r}")
        text = text.replace(anchor, anchor + appended, 1)
    for anchor, prepended in _PREPEND_INJECTIONS:
        n = text.count(anchor)
        if n != 1:
            raise RuntimeError(
                f"inject_counters: anchor appears {n} times, expected 1: "
                f"{anchor!r}")
        text = text.replace(anchor, prepended + anchor, 1)
    return text


def find_cc() -> str:
    find_cc_sh = REPO_ROOT / "launcher" / "tools" / "find_cc.sh"
    out = subprocess.run(
        ["bash", "-c", f'. "{find_cc_sh.as_posix()}"; find_cc'],
        capture_output=True, text=True, check=True)
    cc = out.stdout.strip()
    if not cc:
        raise RuntimeError("no C compiler found (see find_cc.sh)")
    return cc


def extract_ref(ref: str, dest: Path) -> None:
    """git archive <ref> -- launcher/main into `dest`. Never touches this
    worktree's own checked-out state - see this file's own header."""
    proc = subprocess.run(
        ["git", "archive", "--format=tar", ref, "launcher/main"],
        cwd=REPO_ROOT, capture_output=True, check=True)
    with tarfile.open(fileobj=io.BytesIO(proc.stdout)) as tf:
        tf.extractall(dest)


def overlay_current_support_files(tree: Path) -> None:
    """Carry the CURRENT tree's counters infrastructure into the extracted
    ref, so the harness (not just the counter DEFINITIONS) is identical on
    both sides of the comparison - see this file's own header, step 2."""
    app_sand = tree / "launcher" / "main" / "apps" / "sand"
    perf_probe = app_sand / "tools" / "perf_probe"
    perf_probe.mkdir(parents=True, exist_ok=True)
    for name in ("sand_work_counters.h", "sand_work_counters.c"):
        shutil.copyfile(HERE / name, app_sand / name)
    shutil.copyfile(HERE / "counters_scene_main.c",
                     perf_probe / "counters_scene_main.c")


def inject_ref_sand_liquid(tree: Path) -> None:
    sand_liquid = tree / "launcher" / "main" / "apps" / "sand" / "sand_liquid.c"
    text = sand_liquid.read_text(encoding="utf-8")
    sand_liquid.write_text(inject_counters(text), encoding="utf-8")


def build(tree: Path, cc: str, out: Path) -> None:
    app_sand = tree / "launcher" / "main" / "apps" / "sand"
    perf_probe = app_sand / "tools" / "perf_probe"
    main_dir = tree / "launcher" / "main"

    # Required: the actual cross-flow algorithm under test, plus what it
    # cannot link without. Optional: sources that only exist from a later
    # ref onward (palette.c/sand_ui.c were split out of material.c well
    # after some refs this tool needs to reach, e.g. ef87042) - included
    # when present so a newer ref still links clean, skipped when absent
    # rather than failing, since neither is on the water scene's call path
    # (verified: no palette_/sand_ui symbol referenced from sand.c or
    # material.c at ef87042).
    required = [
        perf_probe / "counters_scene_main.c",
        app_sand / "sand.c",
        app_sand / "sand_liquid.c",
        app_sand / "sand_gas.c",
        app_sand / "sand_reactions.c",
        app_sand / "sand_work_counters.c",
        app_sand / "material.c",
        app_sand / "row_runs.c",
        app_sand / "tilt.c",
    ]
    optional = [app_sand / "palette.c", app_sand / "sand_ui.c",
                app_sand / "sand_plants.c"]
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise RuntimeError(f"missing source(s) in extracted tree: {missing}")
    sources = required + [p for p in optional if p.is_file()]

    # Codegen-shaping flags (device_profile.sh's DP_CODEGEN_FLAGS) are
    # deliberately NOT reused here, unlike build_probe.sh: this tool counts
    # source-level events (a SAND_WORK_COUNT() call site executing), which
    # is invariant under inlining, jump-table conversion or any other
    # codegen choice - a counted statement runs the same number of times
    # whichever shape the compiler gives it. build_probe.sh's fidelity
    # concern is about TIMING one specific codegen shape; this tool never
    # times anything.
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-O2", "-g",
           "-DCONFIG_LAUNCHER_DEVELOPMENT=1",
           "-DCONFIG_LAUNCHER_SAND_WORK_COUNTERS=1",
           "-I", str(main_dir), "-I", str(app_sand),
           *[str(s) for s in sources],
           "-o", str(out)]
    subprocess.run(cmd, check=True)


def run_scene(binary: Path, scene: str, steps: int) -> dict:
    proc = subprocess.run([str(binary), scene, str(steps)],
                          capture_output=True, text=True, check=True)
    counters = {}
    in_scene = False
    for line in proc.stdout.splitlines():
        if line.startswith("scene "):
            in_scene = True
            continue
        if not in_scene:
            continue
        parts = line.split()
        if len(parts) == 2:
            counters[parts[0]] = int(parts[1])
    if not counters:
        raise RuntimeError(
            f"no counter output from {binary} - stdout was:\n{proc.stdout}")
    return counters


def one_side(ref: str, scratch_root: Path, cc: str, scene: str,
             steps: int) -> dict:
    tree = scratch_root / "tree"
    tree.mkdir(parents=True)
    extract_ref(ref, tree)
    overlay_current_support_files(tree)
    inject_ref_sand_liquid(tree)
    binary = scratch_root / ("probe.exe" if sys.platform == "win32" else "probe")
    build(tree, cc, binary)
    return run_scene(binary, scene, steps)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref_a")
    ap.add_argument("ref_b", nargs="?",
                    help="defaults to ref_a^ (its parent)")
    ap.add_argument("--scene", default="water")
    ap.add_argument("--steps", type=int, default=20)
    ap.add_argument("--keep", action="store_true",
                    help="do not delete the scratch trees on exit "
                         "(for inspecting a build failure)")
    args = ap.parse_args()

    ref_b = args.ref_b or f"{args.ref_a}^"
    cc = find_cc()

    workdir = Path(tempfile.mkdtemp(prefix="sand_compare_counters_"))
    try:
        print(f"# building {args.ref_a} ...", file=sys.stderr)
        counters_a = one_side(args.ref_a, workdir / "a", cc, args.scene,
                              args.steps)
        print(f"# building {ref_b} ...", file=sys.stderr)
        counters_b = one_side(ref_b, workdir / "b", cc, args.scene,
                              args.steps)
    finally:
        if args.keep:
            print(f"# scratch trees kept at {workdir}", file=sys.stderr)
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    names = list(counters_a.keys())
    for n in counters_b:
        if n not in names:
            names.append(n)

    label_a = args.ref_a[:12]
    label_b = ref_b[:12]
    w = max(len(n) for n in names) if names else 0
    print(f"\nscene {args.scene}, {args.steps} steps")
    print(f"{'counter':<{w}}  {label_a:>12}  {label_b:>12}  {'delta':>10}  {'pct':>8}")
    for n in names:
        va = counters_a.get(n)
        vb = counters_b.get(n)
        if va is None or vb is None:
            print(f"{n:<{w}}  {str(va):>12}  {str(vb):>12}  {'?':>10}  {'?':>8}")
            continue
        delta = vb - va
        pct = (delta / va * 100.0) if va != 0 else float("inf") if delta else 0.0
        print(f"{n:<{w}}  {va:>12}  {vb:>12}  {delta:>+10}  {pct:>+7.1f}%")

    return 0


if __name__ == "__main__":
    sys.exit(main())
