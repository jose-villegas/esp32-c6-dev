#!/usr/bin/env python3
"""Recompiles ONE source file several ways with the real RISC-V compile
command and reports what changed in the generated code - no device, no
full build.

    python launcher/tools/codegen_diff.py <source-file-in-tree> \\
        [--build-dir launcher/build.diag] \\
        --variant "<label>=<extra compiler flags>" \\
        --variant "<label>=@<alternate source path>" \\
        [--symbol sand_step]

WHY THIS EXISTS

Step 1 of a perf round ("attribute before optimising",
docs/Sand/Perf-Round-Guide.md) asks whether a change altered the generated
code at all before spending a device cycle on it. Answering that by eye
means reading two `idf.py build` logs, or worse, trusting that a source
edit did what it looks like it should. This does it directly: pull the
actual compile command for the file out of an existing build's
`compile_commands.json`, run it again - once per `--variant` - and diff the
disassembly.

Ported from this session's own throwaway prototypes (skip_cc.py,
probe_cc.py) after they proved themselves on a real round: they caught a
RISC-V `andi` signed-12-bit-immediate difference that would have
invalidated a scaling experiment, an inlining change that would have made a
second experiment unattributable (the function vanished from one variant's
symbol table entirely - see "symbol not found" below), and a stale build
directory being read as current. This repo's recurring failure mode - the
inlining cliff - has been found four times, every time by objdump.

FOUR GOTCHAS, not three - the fourth was found writing this tool, the other
three were already known from the prototypes:

1. `compile_commands.json` entries carry either `command` (a shell-quoted
   string) or `arguments` (a pre-split list), depending on generator.
   `shlex.split(cmd, posix=False)` handles the string form without
   mangling Windows backslash paths - `posix=True` would treat `\\` as an
   escape character and corrupt every `C:\\...` token.
2. The real command is far past cmd.exe's 8 KB line limit. Every subprocess
   call here passes a list with `shell=False` - never a joined string.
3. CMake escapes an embedded quote for a shell this script never invokes,
   e.g. `-DIDF_VER=\\"v5.5.5\\"`. Stripping the backslash leaves the literal
   quote character GCC's preprocessor wants as part of the macro's string
   value.
4. ESP-IDF also emits a GCC @response-file argument for the architecture
   flags (`@"<build-dir>/toolchain/cflags"`, its own workaround for the
   same 8 KB limit as point 2), wrapped in quotes meant for a shell that
   would have stripped them before GCC ever saw the argument. Passed
   through unparsed, as this script must, the quotes stay literal and GCC
   goes looking for a file named `"...cflags"` - quote marks and all - and
   fails with "linker input file not found". Point 3's fix does not apply
   here: those quotes were never backslash-escaped, so a token whose own
   first two characters are `@"` and whose last character is `"` has its
   wrapping quotes stripped outright, not just un-escaped.

Only the SOURCE FILE path is ever rewritten for an `@variant` (a variant
that substitutes a whole alternate file) - every `-I` keeps pointing at the
real build directory, because that is where the generated `sdkconfig.h`
lives, and this script does not carry its own copy.

WHAT "SYMBOL NOT FOUND" MEANS

Looked up by name in the object's own symbol table, not guessed from a
`.text.<name>` section-naming convention (a specialised clone can live in
`.text.<name>.part.0` etc). If a `--symbol` is missing from one variant
entirely, that is not a bug in this script - it means the function was
inlined away or renamed in that variant, which is itself exactly the
finding a perf round needs to see rather than have swallowed.

This is a Python source-analysis tool - scripts/check-comment-length.sh
does not apply to it.
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

# --- compile_commands.json handling -----------------------------------------

def _fix_token(tok):
    """Undo the two ways compile_commands.json quotes things for a shell
    this script never runs - see gotchas 3 and 4 in the module docstring."""
    tok = tok.replace('\\"', '"')
    if tok.startswith('@"') and tok.endswith('"') and len(tok) > 2:
        tok = "@" + tok[2:-1]
    return tok


def load_compile_command(build_dir, source_file):
    """Returns (directory, argv) for the entry in <build_dir>/
    compile_commands.json whose file matches source_file (matched by
    resolved absolute path, falling back to a path-suffix match so a
    relative in-tree path typed on the command line still finds an
    absolute entry)."""
    cc_path = Path(build_dir) / "compile_commands.json"
    if not cc_path.is_file():
        sys.exit(
            f"codegen_diff: no compile_commands.json under {build_dir!r} - "
            f"pass --build-dir, or build that variant first "
            f"(idf.py -B {build_dir} ... build)"
        )
    db = json.loads(cc_path.read_text(encoding="utf-8", errors="replace"))

    target_abs = os.path.normcase(os.path.abspath(source_file))
    target_suffix = os.path.normcase(source_file.replace("\\", "/"))

    exact, suffix_matches = None, []
    for ent in db:
        entry_file = ent["file"]
        if os.path.normcase(os.path.abspath(entry_file)) == target_abs:
            exact = ent
            break
        if os.path.normcase(entry_file.replace("\\", "/")).endswith(target_suffix):
            suffix_matches.append(ent)

    ent = exact
    if ent is None:
        if len(suffix_matches) == 1:
            ent = suffix_matches[0]
        elif len(suffix_matches) > 1:
            sys.exit(
                f"codegen_diff: {source_file!r} matches {len(suffix_matches)} "
                f"entries in {cc_path} by suffix - pass a longer path"
            )
        else:
            sys.exit(f"codegen_diff: {source_file!r} not found in {cc_path}")

    if "arguments" in ent:
        argv = [_fix_token(a) for a in ent["arguments"]]
    else:
        argv = [_fix_token(a) for a in shlex.split(ent["command"], posix=False)]

    directory = ent.get("directory", str(build_dir))
    print(f"codegen_diff: compile command for {ent['file']} from {cc_path}")
    return directory, argv


# --- objdump ------------------------------------------------------------------

def find_objdump(compiler_path):
    m = re.sub(r"-gcc(\.exe)?$", r"-objdump\1", compiler_path, flags=re.IGNORECASE)
    if m != compiler_path and Path(m).is_file():
        return m
    sys.exit(
        f"codegen_diff: could not derive objdump from compiler path "
        f"{compiler_path!r} (expected it to end in -gcc[.exe]) - pass "
        f"--objdump explicitly"
    )


# A symbol-table line, e.g.:
#   0000001e g     F .text.sand_step\t000014f8 sand_step
# The 7-character flags field's LAST character is the type letter -
# 'F' for function, blank for a section symbol (see objdump(1), "-t").
_SYMTAB_RE = re.compile(
    r"^([0-9a-fA-F]+)\s(.{7})\s(\.\S+)\t([0-9a-fA-F]+)\s+(\S+)\s*$", re.M
)

# One disassembled instruction line, e.g.:
#   "      26:\t2e812c23          \tsw\ts0,760(sp)"
# Never matches a "Disassembly of section ..." or "<label>:" header line -
# those start at column 0, an instruction line is always indented.
_INSTR_RE = re.compile(r"^\s+[0-9a-fA-F]+:\s+[0-9a-fA-F ]+\s+(\S+)(?:\s+(.*))?$", re.M)


def run_objdump(objdump_bin, obj_path, extra_args=()):
    r = subprocess.run(
        [objdump_bin, *extra_args, str(obj_path)],
        capture_output=True, text=True, shell=False
    )
    if r.returncode != 0:
        sys.exit(f"codegen_diff: objdump failed on {obj_path}: {r.stderr.strip()}")
    return r.stdout


def function_symbols(objdump_bin, obj_path):
    """Every .text* FUNCTION symbol as {name: (section, addr, size)}."""
    text = run_objdump(objdump_bin, obj_path, ["-t"])
    out = {}
    for addr, flags, section, size, name in _SYMTAB_RE.findall(text):
        if flags[-1] != "F" or not section.startswith(".text"):
            continue
        out[name] = (section, int(addr, 16), int(size, 16))
    return out


def instructions_in(text):
    return [(m, (ops or "").strip()) for m, ops in _INSTR_RE.findall(text)]


def whole_text_instructions(objdump_bin, obj_path):
    return instructions_in(run_objdump(objdump_bin, obj_path, ["-d"]))


def symbol_instructions(objdump_bin, obj_path, section):
    return instructions_in(
        run_objdump(objdump_bin, obj_path, ["-d", f"--section={section}"])
    )


_FRAME_RE = re.compile(r"^sp,sp,-(\d+)$")


def stack_frame_bytes(instrs):
    for mnem, ops in instrs:
        if mnem != "addi":
            continue
        m = _FRAME_RE.match(ops)
        if m:
            return int(m.group(1))
    return None


def sp_touch_counts(instrs):
    store = sum(
        1 for mnem, ops in instrs
        if re.match(r"^s[bhw]$", mnem) and "(sp)" in ops
    )
    load = sum(
        1 for mnem, ops in instrs
        if re.match(r"^l[bhw]u?$", mnem) and "(sp)" in ops
    )
    return store, load


# --- variants -------------------------------------------------------------

def parse_variant(spec):
    if "=" not in spec:
        sys.exit(f"codegen_diff: --variant {spec!r} must be LABEL=SPEC")
    label, rest = spec.split("=", 1)
    if not label:
        sys.exit(f"codegen_diff: --variant {spec!r} has an empty label")
    return label, rest


def build_variant_argv(base_argv, source_file, spec, out_obj):
    """base_argv already has its -o and source-file tokens; this returns a
    fresh copy with -o repointed at out_obj and, for an @-spec, the source
    file token repointed at the alternate file. Every -I is left untouched -
    see the module docstring on why that matters."""
    argv = list(base_argv)
    for i, tok in enumerate(argv):
        if tok == "-o" and i + 1 < len(argv):
            argv[i + 1] = str(out_obj)

    if spec.startswith("@"):
        alt = str(Path(spec[1:]).resolve())
        source_abs = os.path.normcase(os.path.abspath(source_file))
        replaced = False
        for i, tok in enumerate(argv):
            if os.path.normcase(os.path.abspath(tok)) == source_abs:
                argv[i] = alt
                replaced = True
        if not replaced:
            sys.exit(
                f"codegen_diff: could not find {source_file!r} as a token "
                f"in its own compile command to substitute {alt!r}"
            )
    elif spec:
        argv.extend(shlex.split(spec))
    return argv


def compile_variant(label, argv, directory):
    r = subprocess.run(argv, cwd=directory, capture_output=True, text=True, shell=False)
    return r.returncode == 0, r.stderr


# --- reporting --------------------------------------------------------------

TOP_FUNCTIONS_SHOWN = 8


class Variant(object):
    __slots__ = (
        "label", "obj", "ok", "stderr", "functions", "instrs", "jr_count",
        "symbol",
    )

    def __init__(self, label, obj):
        self.label = label
        self.obj = obj
        self.ok = False
        self.stderr = ""
        self.functions = {}
        self.instrs = []
        self.jr_count = 0
        self.symbol = None  # (section, size, instrs, frame, sp_store, sp_load) or None


def report_variant(v):
    print(f"\n=== {v.label} ===")
    if not v.ok:
        print(f"  COMPILE FAILED: {v.stderr.strip()[:400]}")
        return
    print(f"  .text instructions: {len(v.instrs)}")
    print(f"  indexed jumps (jr):  {v.jr_count}")
    by_size = sorted(v.functions.items(), key=lambda kv: -kv[1][2])
    print(f"  largest {min(TOP_FUNCTIONS_SHOWN, len(by_size))} functions:")
    for name, (_section, _addr, size) in by_size[:TOP_FUNCTIONS_SHOWN]:
        print(f"    {size:6d}  {name}")

    if v.symbol is None:
        return
    section, size, instrs, frame, sp_store, sp_load = v.symbol
    if instrs is None:
        print(f"  symbol: NOT FOUND (inlined away or renamed in this variant)")
        return
    print(f"  symbol {section}:")
    print(f"    size:              {size} bytes")
    print(f"    stack frame:       {frame if frame is not None else 'none found'} bytes")
    print(f"    sp-relative stores: {sp_store}")
    print(f"    sp-relative loads:  {sp_load}")


def diff_instrs(a, b, limit=6):
    """Index-aligned diff of two instruction-sequences. Returns
    (same_length, differing_count, first_differences). differing_count
    counts every index where the two sequences disagree, over their common
    length, PLUS the length difference itself (a trailing extra/missing
    instruction counts as one more difference) - so two sequences of
    different length are never reported as "0 differences"."""
    n = min(len(a), len(b))
    diffs = [(i, a[i], b[i]) for i in range(n) if a[i] != b[i]]
    differing = len(diffs) + abs(len(a) - len(b))
    return len(a) == len(b), differing, diffs[:limit]


def report_pair(base, other, symbol):
    print(f"\n--- {base.label} vs {other.label} ---")
    if not (base.ok and other.ok):
        print("  skipped (one side failed to compile)")
        return
    same_n, differing, _ = diff_instrs(base.instrs, other.instrs)
    print(
        f"  verdict: instruction counts {'MATCH' if same_n else 'DIFFER'} "
        f"({len(base.instrs)} vs {len(other.instrs)}); "
        f"{differing} differing instruction line(s) (whole .text)"
    )

    if not symbol:
        return
    b_instrs = base.symbol[2] if base.symbol else None
    o_instrs = other.symbol[2] if other.symbol else None
    if b_instrs is None or o_instrs is None:
        print(f"  symbol {symbol}: not present in both variants, no diff to show")
        return
    same_n, differing, first = diff_instrs(b_instrs, o_instrs)
    print(
        f"  symbol {symbol}: {differing} differing instruction line(s) "
        f"({len(b_instrs)} vs {len(o_instrs)} total)"
    )
    for i, x, y in first:
        print(f"    @{i}  {x[0]} {x[1]}   |   {y[0]} {y[1]}")


# --- main --------------------------------------------------------------------

def main(argv):
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", help="source file to recompile, as it appears in the tree")
    parser.add_argument("--build-dir", default="launcher/build.diag",
                         help="build directory holding compile_commands.json "
                              "(default: launcher/build.diag)")
    parser.add_argument("--variant", action="append", default=[], required=True,
                         metavar="LABEL=SPEC",
                         help="LABEL=extra-flags, or LABEL=@alternate-source-path; "
                              "repeatable. The first --variant is the baseline "
                              "every later one is compared against.")
    parser.add_argument("--symbol", help="also report this function's size/frame/"
                                          "sp-touch stats and diff its own instructions")
    parser.add_argument("--objdump", help="override the auto-derived objdump path")
    args = parser.parse_args(argv)

    directory, base_argv = load_compile_command(args.build_dir, args.source)
    compiler = base_argv[0]
    objdump_bin = args.objdump or find_objdump(compiler)

    tmp_dir = Path(tempfile.mkdtemp(prefix="codegen_diff_"))
    print(f"codegen_diff: objdump = {objdump_bin}")
    print(f"codegen_diff: scratch object files in {tmp_dir}")

    variants = []
    for spec in args.variant:
        label, rest = parse_variant(spec)
        obj = tmp_dir / f"{label}.o"
        v_argv = build_variant_argv(base_argv, args.source, rest, obj)
        ok, stderr = compile_variant(label, v_argv, directory)
        v = Variant(label, obj)
        v.ok, v.stderr = ok, stderr
        if ok:
            v.functions = function_symbols(objdump_bin, obj)
            v.instrs = whole_text_instructions(objdump_bin, obj)
            v.jr_count = sum(1 for m, _ in v.instrs if m == "jr")
            if args.symbol:
                sym = v.functions.get(args.symbol)
                if sym is None:
                    v.symbol = (None, None, None, None, None, None)
                else:
                    section, _addr, size = sym
                    sym_instrs = symbol_instructions(objdump_bin, obj, section)
                    frame = stack_frame_bytes(sym_instrs)
                    sp_store, sp_load = sp_touch_counts(sym_instrs)
                    v.symbol = (section, size, sym_instrs, frame, sp_store, sp_load)
        variants.append(v)

    for v in variants:
        report_variant(v)

    if len(variants) >= 2:
        base = variants[0]
        for other in variants[1:]:
            report_pair(base, other, args.symbol)

    return 0 if all(v.ok for v in variants) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
