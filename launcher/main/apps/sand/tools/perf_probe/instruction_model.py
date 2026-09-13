#!/usr/bin/env python3
"""instruction_model.py - where a step's INSTRUCTIONS go, without a device.

The C6's own L1 counters settled that the sim is instruction-bound: 89 fetch
misses and no data misses in a 2.3 million cycle water step, with the fetch
unit busy 0.89 cycles in every one (docs/sand/Perf-
Instruments.md). So a pass costs what it executes, and the useful question
about any hot loop is how many instructions it runs - which needs two halves
that this script joins:

  riscv instructions per source line   objdump -dl on build.diag's own .obj
  times
  executions per source line           gcov, running the same scene on a host

WHAT THE DIVISION BY SITES IS FOR, because the naive product is wrong by 2x.
One source line lands at several places in the object - a loop versioned for
its two directions, a tail GCC duplicated, a `static inline` expanded at
fifteen call sites - and gcov reports the SUM of their executions. Multiplying
total-instructions-at-a-line by total-executions-of-that-line squares that
multiplicity. Dividing by the number of distinct instruction runs the line
owns removes it, on the assumption the copies run equally often.

WHAT IT IS WORTH, measured against the device's fetch counter on the water
scene, 2026-09-11:

  ref        model insn/step   device ibus/step   error
  4d9efa7        1,918,761          1,961,884     -2.2%
  01d12db        2,122,082          2,047,557     +3.6%

So the SHAPE of a step is trustworthy: it put cross-flow at 48% of the water
step against the gated decomposition's 51%, and ranked the two grid walks
first and second, which is what a round needs to choose a target.

WHAT IT IS NOT WORTH, and this is the half that costs a day if ignored:
DIFFERENCING TWO RUNS OF THIS MODEL DOES NOT PRICE A CANDIDATE. Across the
pair above the model predicts -203,321 instructions where the device measured
-85,673 fetches - 2.4x over - because the +3.6% and the -2.2% are independent
attribution errors of their own and their difference is 5.8% of a 2 million
instruction step, wider than most effects worth chasing. A change also moves
line attribution around: the same pair reads -171,276 on equalise_one_row_cell
and +56,013 on equalise_one_block, which is one function's instructions being
re-labelled as another's.

PRICE A CANDIDATE THE OTHER WAY: with the host work counters (counters_
scene_main.c, next door) for how many times the work happens, and a hand count
of the EXECUTED path in the disassembly for what one occurrence costs. Weight
each exit by how often it is taken, and include whatever the change pushes
back into its caller - leaving that out is most of why #174's own static model
predicted 9-10% and the device gave 4.0%.

USAGE

    python instruction_model.py [--scene water] [--steps 20] [--top 30] \\
        [--build-dir launcher/build.diag] [--work-dir <scratch>]

Needs a build.diag that has already been built (capture_ref.sh --build-only
--perf-scope is the cheap way), the riscv toolchain's objdump on PATH or at
the usual Espressif location, and a host gcc with gcov.
"""
import argparse
import collections
import gzip
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
APP_SAND = HERE.parent.parent
MAIN_DIR = APP_SAND.parent.parent
REPO_ROOT = MAIN_DIR.parent.parent

# Every portable source of the app. Missing ones are skipped rather than
# fatal, so the script still runs against a ref that predates one.
TUS = ["sand", "sand_liquid", "sand_gas", "sand_reactions", "sand_plants",
       "sand_impulse", "material", "palette", "material_palette", "row_runs",
       "sand_ui", "tilt"]

LOC = re.compile(r"^(.*?):(\d+)(?: \(discriminator \d+\))?$")
INSN = re.compile(r"^\s*[0-9a-f]+:\t")
FUNC = re.compile(r"^[A-Za-z_][A-Za-z0-9_ *]*\**\s*[A-Za-z_][A-Za-z0-9_]*\s*\(")


def find_objdump():
    found = shutil.which("riscv32-esp-elf-objdump")
    if found:
        return found
    roots = sorted(pathlib.Path.home().glob(
        ".espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin"))
    for root in reversed(roots):
        for name in ("riscv32-esp-elf-objdump.exe", "riscv32-esp-elf-objdump"):
            if (root / name).is_file():
                return str(root / name)
    raise SystemExit("no riscv32-esp-elf-objdump found - run ESP-IDF's export "
                     "script, or put the toolchain's bin on PATH")


def find_cc():
    out = subprocess.run(
        ["bash", "-c", f'. "{(MAIN_DIR.parent / "tools" / "find_cc.sh").as_posix()}"; find_cc'],
        capture_output=True, text=True, check=False)
    cc = out.stdout.strip()
    if not cc:
        raise SystemExit("no host C compiler found (see launcher/tools/find_cc.sh)")
    return cc


def insns_per_line(path):
    """(basename, line) -> (instructions, distinct code sites)."""
    counts, sites, cur, prev = collections.Counter(), collections.Counter(), None, None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if INSN.match(line):
                if cur:
                    counts[cur] += 1
                    if prev != cur:
                        sites[cur] += 1
                    prev = cur
                continue
            if line.endswith("():") or not line.strip():
                continue
            m = LOC.match(line)
            if m and ("/" in m.group(1) or "\\" in m.group(1)):
                cur = (os.path.basename(m.group(1)), int(m.group(2)))
    return {k: (v, sites[k]) for k, v in counts.items()}


def counts_per_line(covdir, tu):
    subprocess.run(["gcov", "-i", "-j", f"{tu}.gcda"], cwd=covdir,
                   capture_output=True, check=True)
    with gzip.open(pathlib.Path(covdir) / f"{tu}.gcov.json.gz", "rt",
                   encoding="utf-8") as fh:
        data = json.load(fh)
    out = {}
    for f in data["files"]:
        base = os.path.basename(f["file"])
        for ln in f["lines"]:
            out[(base, ln["line_number"])] = ln["count"]
    return out


def function_map():
    """(basename, line) -> enclosing function, by brace scanning the sources."""
    owner = {}
    for p in sorted(APP_SAND.rglob("*")):
        if p.suffix not in (".c", ".h"):
            continue
        lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
        name, depth = None, 0
        for i, text in enumerate(lines, 1):
            if depth == 0 and FUNC.match(text):
                name = re.findall(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", text)[0]
            if name:
                owner[(p.name, i)] = name
            depth = max(0, depth + text.count("{") - text.count("}"))
            if depth == 0 and text.startswith("}"):
                name = None
    return owner


def dump_disassembly(build_dir, outdir):
    objdump = find_objdump()
    objdir = (pathlib.Path(build_dir) / "esp-idf" / "main" / "CMakeFiles" /
              "__idf_main.dir" / "apps" / "sand")
    if not objdir.is_dir():
        raise SystemExit(f"no compiled objects at {objdir} - build build.diag first")
    for tu in TUS:
        obj = objdir / f"{tu}.c.obj"
        if not obj.is_file():
            continue
        with open(outdir / f"{tu}.dis", "w", encoding="utf-8") as fh:
            subprocess.run([objdump, "-dl", str(obj)], stdout=fh, check=True)


def run_coverage(covdir, scene, steps):
    cc = find_cc()
    flags = ["-std=c11", "-O2", "-g", "--coverage",
             "-DCONFIG_LAUNCHER_DEVELOPMENT=1",
             "-I", str(MAIN_DIR), "-I", str(APP_SAND)]
    objs = []
    for tu in TUS:
        src = APP_SAND / f"{tu}.c"
        if not src.is_file():
            continue
        subprocess.run([cc, *flags, "-c", str(src), "-o", f"{tu}.o"],
                       cwd=covdir, check=True)
        objs.append(f"{tu}.o")
    subprocess.run([cc, *flags, "-c", str(HERE / "model_scene_main.c"),
                    "-o", "model_scene_main.o"], cwd=covdir, check=True)
    objs.append("model_scene_main.o")
    subprocess.run([cc, "--coverage", *objs, "-o", "model_probe"],
                   cwd=covdir, check=True)
    binary = pathlib.Path(covdir) / "model_probe"
    if not binary.exists():
        binary = pathlib.Path(covdir) / "model_probe.exe"
    subprocess.run([str(binary), scene, str(steps)], cwd=covdir, check=True)


def report(disdir, covdir, steps, top):
    fmap = function_map()
    rows, per_tu, grand = [], {}, 0.0
    for dis in sorted(pathlib.Path(disdir).glob("*.dis")):
        tu = dis.stem
        if not (pathlib.Path(covdir) / f"{tu}.gcda").exists():
            continue
        counts = counts_per_line(covdir, tu)
        total = 0.0
        for key, (n_ins, n_sites) in insns_per_line(dis).items():
            c = counts.get(key, 0)
            if not c:
                continue
            est = n_ins / n_sites * c
            total += est
            rows.append((est, tu, key[0], key[1], n_ins, n_sites, c,
                         fmap.get(key, "?")))
        per_tu[tu] = total
        grand += total

    print(f"{'translation unit':<20}{'insn/step':>13}{'share':>8}")
    for tu, t in sorted(per_tu.items(), key=lambda kv: -kv[1]):
        if t:
            print(f"{tu:<20}{t / steps:>13,.0f}{t / grand * 100:>7.1f}%")
    print(f"{'TOTAL':<20}{grand / steps:>13,.0f}\n")

    byfn = collections.Counter()
    for est, _tu, _f, _ln, _ni, _ns, _c, fn in rows:
        byfn[fn] += est
    print(f"{'insn/step':>11}{'share':>8}  function")
    for fn, t in byfn.most_common(top):
        print(f"{t / steps:>11,.0f}{t / grand * 100:>7.1f}%  {fn}")

    print(f"\n{'insn/step':>11} {'file:line':<26}{'ins':>5}{'sites':>6}"
          f"{'exec/step':>11}  function")
    rows.sort(reverse=True)
    for est, _tu, f, ln, ni, ns, c, fn in rows[:top]:
        print(f"{est / steps:>11,.0f} {f + ':' + str(ln):<26}{ni:>5}{ns:>6}"
              f"{c / steps:>11,.0f}  {fn}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", default="water", choices=["water", "sand"])
    ap.add_argument("--steps", type=int, default=20)
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--build-dir",
                    default=str(MAIN_DIR.parent / "build.diag"),
                    help="a built build.diag (default: launcher/build.diag)")
    ap.add_argument("--work-dir", default=None,
                    help="where to put the scratch objects (default: a temp dir)")
    a = ap.parse_args()

    work = pathlib.Path(a.work_dir) if a.work_dir else pathlib.Path(
        tempfile.mkdtemp(prefix="sand_insn_model_"))
    disdir, covdir = work / "dis", work / "cov"
    disdir.mkdir(parents=True, exist_ok=True)
    covdir.mkdir(parents=True, exist_ok=True)

    dump_disassembly(a.build_dir, disdir)
    run_coverage(covdir, a.scene, a.steps)
    report(disdir, covdir, a.steps, a.top)
    print(f"\nscratch: {work}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
