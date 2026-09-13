#!/usr/bin/env python3
"""reaction_coverage.py - which reaction slots does anything actually fire?

Answers "which (material, reaction_t field) slots does the benchmark set
actually exercise", by instrumenting the real firing sites in the sand
engine, running scenes through this directory's own host probe, and
reporting fired slots against M - the total the material tables define.
Built to answer a concrete question this session hit while attributing a
perf round: the benchmark set turned out to cover 60 of 80 slots, and 16 of
the missing 20 were the entire plant lifecycle - evidence a plants perf pass
had been waiting for. It also proved the gunpowder fuse
could never fire in the benchmark set, which turned out to be a missing
sand_enable_impulses() call rather than scene geometry.

WHAT THIS SCRIPT DOES

  1. `git archive <ref> -- launcher/main` into a fresh scratch tree - never
     `git checkout <ref>` in this worktree, and never edits sand_reactions.c
     / sand_plants.c in place here. Same reasoning as compare_counters.py's
     own header.
  2. Overlays reaction_slots.h/.c (the slot storage) and this directory's
     own coverage_scene_main.c/gfx_probe_stub.c/esp_timer_host.c into the
     scratch tree - the harness support files, carried in unchanged so the
     harness itself is never a variable.
  3. Runs apply_edits.py over the scratch tree's apps/sand/ directory. That
     script takes a FILE OR A DIRECTORY and places each edit in whichever
     file actually contains its anchor - see its own docstring for why a
     source split (sand_reactions.c -> sand_plants.c, and whatever comes
     next) needs no anchor rewriting here. It exits non-zero if any edit
     finds no home, and this script treats that as fatal: an unplaced edit
     means those reactions are never counted, and a coverage report that
     silently swallowed that would print "never fires" for something that
     was simply never watched - worse than no report at all.
  4. Builds the coverage probe (coverage_scene_main.c plus the real,
     unmodified suite_sand_*.c scene builders and the algorithm sources they
     drive - the same sources build_probe.sh links, minus probe_main.c,
     plus reaction_slots.c) and count_slots (the denominator: it walks
     reactions[]/extended_reactions[] the way dump_reactions.c's
     build_rows() does, entirely independent of the instrumentation above).
  5. Runs each requested scene as its own process (clean slot_hit state per
     scene - simpler than resetting statics mid-process), parses the fired
     slots out of each run's own sentinel-delimited report, and prints
     per-scene coverage, the union across every scene run, and the
     complement - M minus the union, the slots nothing fired. The complement
     is the output people actually act on.

This build deliberately does NOT read the device profile's codegen-shaping
flags the way build_probe.sh does. Those flags exist to make a TIMED host
number predict device cost; this script never times anything; it counts
whether a slot_note_r() call site executed at least once, which is
invariant under inlining, jump-table conversion, or any other codegen
choice a compiler makes - see compare_counters.py's own header for the twin
reasoning about its work counters.

USAGE

    python reaction_coverage.py [SCENE ...] [--ref REF] [--keep]

With no scene named, runs every scene this probe knows (coverage_scene_main.c
--list). --ref defaults to HEAD.
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
TEST_DIR = MAIN_DIR.parent / "test"     # launcher/test

# Keep in sync with coverage_scene_main.c's own SCENES table (itself a
# deliberate duplicate of probe_main.c's - see both files' own comments on
# why nothing discovers these automatically). Used only as the DEFAULT scene
# list; --list on the built binary is the source of truth if this ever
# drifts.
DEFAULT_SCENES = [
    "full_step_control", "settled_flip_control", "water", "mixed_flip",
    "settled_pool_to_landscape", "lava_stress", "four_liquids", "wet_earth",
    "water_over_lava", "every_material_flip", "smoke_and_steam",
    "gunpowder_basin",
]


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
    """git archive <ref> -- launcher/main launcher/test into `dest`. Never
    touches this worktree's own checked-out state - see this file's own
    header. Both directories are needed (unlike compare_counters.py, which
    only ever needed launcher/main): the probe's own suites.c/timing.c and
    the vendored Unity framework live in launcher/test, a sibling of
    launcher/main, not under it."""
    proc = subprocess.run(
        ["git", "archive", "--format=tar", ref,
         "launcher/main", "launcher/test"],
        cwd=REPO_ROOT, capture_output=True, check=True)
    with tarfile.open(fileobj=io.BytesIO(proc.stdout)) as tf:
        tf.extractall(dest)


def overlay_support_files(tree: Path) -> tuple[Path, Path]:
    """Carry the harness support files (reaction_slots.* plus this probe's
    own main/link stubs) into the extracted ref - the harness must be
    identical regardless of which ref is being measured."""
    app_sand = tree / "launcher" / "main" / "apps" / "sand"
    perf_probe = app_sand / "tools" / "perf_probe"
    perf_probe.mkdir(parents=True, exist_ok=True)
    for name in ("reaction_slots.h", "reaction_slots.c"):
        shutil.copyfile(HERE / name, app_sand / name)
    for name in ("coverage_scene_main.c", "gfx_probe_stub.c",
                 "esp_timer_host.c", "count_slots.c"):
        shutil.copyfile(HERE / name, perf_probe / name)
    return app_sand, perf_probe


def run_apply_edits(app_sand: Path) -> None:
    """Instruments the scratch tree's sand_reactions.c/sand_plants.c in
    place - see this file's own header, step 3. Fails loudly (not just a
    non-zero exit swallowed by check=True's CalledProcessError) so an
    under-applied instrumentation pass is never mistaken for "these
    reactions never fire"."""
    proc = subprocess.run(
        [sys.executable, str(HERE / "apply_edits.py"), str(app_sand)],
        capture_output=True, text=True)
    sys.stdout.write(proc.stdout)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise RuntimeError(
            "apply_edits.py could not place every slot edit (see output "
            "above) - refusing to report coverage from a partially-"
            "instrumented tree. An unplaced edit means those reactions are "
            "never MEASURED, not that they never fire; fix the anchor "
            "before trusting any number below.")


def build_coverage_probe(tree: Path, app_sand: Path, perf_probe: Path,
                          cc: str, out: Path) -> None:
    main_dir = tree / "launcher" / "main"
    test_dir = tree / "launcher" / "test"

    # The same algorithm + scene-builder sources build_probe.sh links,
    # minus probe_main.c (coverage_scene_main.c replaces it), plus
    # reaction_slots.c (the counters this build actually cares about).
    sources = [
        perf_probe / "coverage_scene_main.c",
        perf_probe / "gfx_probe_stub.c",
        perf_probe / "esp_timer_host.c",
        test_dir / "suites.c",
        test_dir / "timing.c",
        app_sand / "sand.c",
        app_sand / "sand_impulse.c",
        app_sand / "sand_liquid.c",
        app_sand / "sand_gas.c",
        app_sand / "sand_reactions.c",
        app_sand / "sand_plants.c",
        app_sand / "material.c",
        app_sand / "material_palette.c",
        app_sand / "palette.c",
        app_sand / "row_runs.c",
        app_sand / "sand_ui.c",
        app_sand / "tilt.c",
        app_sand / "reaction_slots.c",
    ]
    for f in sorted(app_sand.glob("suite_sand_*.c")):
        if f.name == "suite_sand_ui.c":
            continue  # never part of what this probe needed - see build_probe.sh
        sources.append(f)

    missing = [str(p) for p in sources if not p.is_file()]
    if missing:
        raise RuntimeError(f"missing source(s) in extracted tree: {missing}")

    cflags = ["-std=gnu17", "-Wall", "-Wextra", "-Wno-unused-parameter",
              "-O0", "-g"]
    defs = ["-DDEVICE_BUILD", "-DSAND_HOST_PROBE",
            "-DCONFIG_LAUNCHER_DEVELOPMENT=1"]
    incs = ["-I", str(main_dir), "-I", str(test_dir),
            "-I", str(test_dir / "framework"), "-I", str(test_dir / "stubs"),
            "-I", str(app_sand)]

    out.parent.mkdir(parents=True, exist_ok=True)

    # unity.c compiled alone, without -include timing.h, same reason
    # build_probe.sh does it - see that script's own comment.
    unity_obj = out.parent / "coverage_unity.o"
    subprocess.run([cc, *cflags, *incs, "-c",
                     str(test_dir / "framework" / "unity.c"),
                     "-o", str(unity_obj)], check=True)

    subprocess.run([cc, *cflags, *defs, *incs,
                     "-include", str(test_dir / "timing.h"),
                     *[str(s) for s in sources], str(unity_obj),
                     "-o", str(out)], check=True)
    unity_obj.unlink(missing_ok=True)


def build_count_slots(tree: Path, app_sand: Path, perf_probe: Path,
                       cc: str, out: Path) -> None:
    """The denominator, entirely independent of the instrumentation above -
    count_slots.c walks the ref's OWN material tables (unmodified by
    apply_edits.py) the way dump_reactions.c's build_rows() does."""
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-O2",
                     "-I", str(app_sand),
                     str(perf_probe / "count_slots.c"),
                     str(app_sand / "material.c"),
                     "-o", str(out)], check=True)


def run_count_slots(binary: Path) -> tuple[int, set]:
    proc = subprocess.run([str(binary)], capture_output=True, text=True,
                           check=True)
    slots = {line.strip() for line in proc.stdout.splitlines() if line.strip()}
    m_line = [l for l in proc.stderr.splitlines() if l.startswith("M = ")]
    if not m_line:
        raise RuntimeError(f"count_slots produced no 'M = ' line - "
                            f"stderr was:\n{proc.stderr}")
    m = int(m_line[0].split("=", 1)[1].strip())
    if m != len(slots):
        raise RuntimeError(f"count_slots printed M={m} but {len(slots)} "
                            f"distinct slot lines - the tool disagrees with "
                            f"itself, do not trust this run")
    return m, slots


def run_scene(binary: Path, scene: str) -> set:
    proc = subprocess.run([str(binary), scene], capture_output=True,
                           text=True, check=True)
    lines = proc.stdout.splitlines()
    try:
        start = lines.index("===SLOTS===")
        end = lines.index("===END SLOTS===")
    except ValueError as exc:
        raise RuntimeError(
            f"coverage probe for scene '{scene}' printed no slot report - "
            f"stdout was:\n{proc.stdout}") from exc
    body = lines[start + 1:end]
    total_lines = [l for l in body if l.startswith("TOTAL ")]
    if len(total_lines) != 1:
        raise RuntimeError(f"scene '{scene}': expected exactly one TOTAL "
                            f"line, got {len(total_lines)}: {total_lines}")
    reported = int(total_lines[0].split()[1])
    slots = {l for l in body if l != total_lines[0]}
    if len(slots) != reported:
        raise RuntimeError(
            f"scene '{scene}': slot_dump said TOTAL {reported} but printed "
            f"{len(slots)} distinct slot lines - do not trust this run")
    return slots


def list_scenes(binary: Path) -> list:
    proc = subprocess.run([str(binary), "--list"], capture_output=True,
                           text=True, check=True)
    return [l.strip() for l in proc.stdout.splitlines() if l.strip()]


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenes", nargs="*",
                     help="scene names to run (default: every scene the "
                          "probe knows)")
    ap.add_argument("--ref", default="HEAD",
                     help="git ref to measure coverage for (default: HEAD)")
    ap.add_argument("--keep", action="store_true",
                     help="do not delete the scratch tree on exit (for "
                          "inspecting a build failure)")
    args = ap.parse_args()

    cc = find_cc()
    workdir = Path(tempfile.mkdtemp(prefix="sand_reaction_coverage_"))
    try:
        tree = workdir / "tree"
        print(f"# archiving {args.ref} ...", file=sys.stderr)
        extract_ref(args.ref, tree)

        app_sand, perf_probe = overlay_support_files(tree)

        print("# instrumenting reaction firing sites ...", file=sys.stderr)
        run_apply_edits(app_sand)

        print("# building coverage probe ...", file=sys.stderr)
        probe_bin = workdir / ("coverage_probe.exe" if sys.platform == "win32"
                                else "coverage_probe")
        build_coverage_probe(tree, app_sand, perf_probe, cc, probe_bin)

        print("# building count_slots (denominator) ...", file=sys.stderr)
        count_bin = workdir / ("count_slots.exe" if sys.platform == "win32"
                                else "count_slots")
        build_count_slots(tree, app_sand, perf_probe, cc, count_bin)

        m, all_slots = run_count_slots(count_bin)

        scenes = args.scenes or list_scenes(probe_bin)
        unknown = [s for s in scenes if s not in list_scenes(probe_bin)]
        if unknown:
            raise RuntimeError(f"unknown scene(s): {', '.join(unknown)} "
                                f"(try with no scenes to see the full list)")

        print(f"\nreaction coverage against {args.ref}, M={m} slots defined\n")

        per_scene = {}
        union = set()
        for scene in scenes:
            fired = run_scene(probe_bin, scene)
            per_scene[scene] = fired
            union |= fired
            pct = (100.0 * len(fired) / m) if m else 0.0
            print(f"  {scene:<28} {len(fired):3d}/{m}  ({pct:5.1f}%)")

        complement = sorted(all_slots - union)
        print(f"\nunion across {len(scenes)} scene(s): {len(union)}/{m} "
              f"({100.0 * len(union) / m if m else 0.0:.1f}%)")
        print(f"complement (never fired by anything run above): "
              f"{len(complement)}/{m}")
        for slot in complement:
            print(f"  {slot}")

    finally:
        if args.keep:
            print(f"# scratch tree kept at {workdir}", file=sys.stderr)
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
