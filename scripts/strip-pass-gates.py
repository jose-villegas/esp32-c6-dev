#!/usr/bin/env python
"""Remove the sand pass gates from the tree, all of them, in one pass.

    python scripts/strip-pass-gates.py [--check]

The gates are measurement scaffolding (docs/sand/Perf-Round-Guide.md, section
"Instrumenting a round"). They compile to nothing once
CONFIG_LAUNCHER_SAND_PASS_GATES is off, so a round can END with the source
carrying none of them and the program unchanged.

WHY A SCRIPT AND NOT A CAREFUL EDIT. There are dozens of call sites across
four files. Unwrapping them by hand is dozens of chances to delete a line
that was doing something, and this has already happened once in this
project's history: a sed with a mangled backreference silently removed three
real calls, everything still compiled, and grep reported the tree clean. All
or nothing, mechanically, is the only safe shape.

WHAT PROVES IT WORKED is not this script. It is
scripts/verify-gate-strip.sh, which builds the before and after refs with
gates OFF and compares the DISASSEMBLY of the sand translation units. Gated
source with gates off and stripped source are supposed to be the same
program; that script is what turns "supposed to" into evidence. Always run
it afterwards.

WHAT THIS DOES NOT DO: comments. Prose that mentions the gates goes stale
when they go, but a comment emits no code, so the object-code check cannot
police it - which makes automatic removal exactly the wrong tool. The script
lists what it found instead, for a human to read and cut.
"""
import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# suite_sand_split.h is DELIBERATELY ABSENT: the decomposition harness is
# permanent tooling that survives a strip, unlike the gates it drives. Adding
# it here would delete it at the end of every round. It guards itself on the
# same option, so a stripped tree compiles it away without this script's help.
SOURCES = [
    "launcher/main/apps/sand/sand.c",
    "launcher/main/apps/sand/sand_gas.c",
    "launcher/main/apps/sand/sand_liquid.c",
    "launcher/main/apps/sand/sand_liquid_move.h",
    "launcher/main/apps/sand/sand_reactions.c",
    "launcher/main/apps/sand/sand_priv.h",
    "launcher/main/apps/sand/suite_sand_perf.c",
]

OPTION = "CONFIG_LAUNCHER_SAND_PASS_GATES"

# An #if whose truth turns on the option. Both forms appear: the option
# alone, and ANDed with DEVICE_BUILD in the perf suite. With the option
# undefined both are false, so both blocks lose their #if branch.
IF_ON_OPTION = re.compile(r"^\s*#\s*if\b.*\b" + OPTION + r"\b")
ANY_IF = re.compile(r"^\s*#\s*if")
ANY_ELSE = re.compile(r"^\s*#\s*el(se|if)\b")
ANY_ENDIF = re.compile(r"^\s*#\s*endif")


def drop_option_blocks(lines):
    """Resolve #if <option> ... [#else ...] #endif as if the option were off.

    Nesting-aware: only the conditionals that actually test the option are
    resolved, and any #if inside them is carried along untouched. Getting
    this wrong in the other direction - matching the first #endif - would
    silently truncate a function.
    """
    out = []
    i = 0
    while i < len(lines):
        if IF_ON_OPTION.match(lines[i]):
            depth = 0
            j = i + 1
            else_at = None
            end_at = None
            while j < len(lines):
                if ANY_IF.match(lines[j]):
                    depth += 1
                elif ANY_ENDIF.match(lines[j]):
                    if depth == 0:
                        end_at = j
                        break
                    depth -= 1
                elif depth == 0 and ANY_ELSE.match(lines[j]) and else_at is None:
                    else_at = j
                j += 1
            if end_at is None:
                raise SystemExit("unterminated #if on %s near line %d" % (OPTION, i + 1))
            if else_at is not None:
                out.extend(lines[else_at + 1:end_at])
            i = end_at + 1
            continue
        out.append(lines[i])
        i += 1
    return out


def split_macro_args(text, start):
    """Given text and the index of '(' after a macro name, return
    (args, index just past the matching ')'). Splits on top-level commas."""
    assert text[start] == "("
    depth = 0
    args = []
    cur = []
    i = start
    while i < len(text):
        ch = text[i]
        if ch == "(":
            depth += 1
            if depth == 1:
                i += 1
                continue
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append("".join(cur))
                return args, i + 1
        elif ch == "," and depth == 1:
            args.append("".join(cur))
            cur = []
            i += 1
            continue
        cur.append(ch)
        i += 1
    raise SystemExit("unbalanced parentheses in a gate macro")


def strip_gated(text):
    """SAND_STEP_GATED(name, EXPR) -> EXPR, however many lines EXPR spans."""
    n = 0
    while True:
        at = text.find("SAND_STEP_GATED(")
        if at < 0:
            return text, n
        open_paren = at + len("SAND_STEP_GATED")
        args, end = split_macro_args(text, open_paren)
        if len(args) < 2:
            raise SystemExit("SAND_STEP_GATED with one argument at offset %d" % at)
        expr = ",".join(args[1:]).strip()
        text = text[:at] + expr + text[end:]
        n += 1


def strip_gate(lines):
    """Remove SAND_STEP_GATE(name), keeping whatever it guarded.

    Three shapes occur, and the braces matter: shape A exists only because
    the macro needed a body, so its braces come out and the body dedents;
    shape B guards a statement that has its own braces, so only the macro
    line goes.
    """
    out = []
    n = 0
    i = 0
    one_liner = re.compile(r"^(\s*)SAND_STEP_GATE\([A-Za-z0-9_]+\)\s*\{\s*(.*?)\s*\}\s*$")
    opens_block = re.compile(r"^(\s*)SAND_STEP_GATE\([A-Za-z0-9_]+\)\s*\{\s*$")
    bare = re.compile(r"^(\s*)SAND_STEP_GATE\([A-Za-z0-9_]+\)\s*$")

    while i < len(lines):
        line = lines[i]

        m = one_liner.match(line)
        if m:                                   # C: guard and body on one line
            out.append(m.group(1) + m.group(2))
            n += 1
            i += 1
            continue

        m = opens_block.match(line)
        if m:                                   # A: guard owns the braces
            indent = m.group(1)
            depth = 1
            j = i + 1
            body = []
            while j < len(lines):
                depth += lines[j].count("{") - lines[j].count("}")
                if depth == 0:
                    break
                body.append(lines[j])
                j += 1
            if depth != 0:
                raise SystemExit("unbalanced braces after SAND_STEP_GATE at line %d" % (i + 1))
            for b in body:
                out.append(b[4:] if b.startswith(indent + "    ") else b)
            n += 1
            i = j + 1                           # skip the closing brace too
            continue

        m = bare.match(line)
        if m:                                   # B: guard precedes a statement
            n += 1
            i += 1
            continue

        out.append(line)
        i += 1
    return out, n


def strip_leftover_macros(text):
    """The no-op macro definitions the #else branch leaves behind."""
    text = text.replace("#define SAND_STEP_GATE(name)\n", "")
    text = text.replace("#define SAND_STEP_GATED(name, cond) (cond)\n", "")
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="report what would change and exit non-zero if anything would")
    args = ap.parse_args()

    total = {"blocks": 0, "gated": 0, "gate": 0}
    changed = []

    for rel in SOURCES:
        path = REPO / rel
        original = path.read_text(encoding="utf-8", newline="")
        lines = original.split("\n")

        before = len(lines)
        lines = drop_option_blocks(lines)
        total["blocks"] += before - len(lines)

        # BEFORE strip_gated, not after. The #else branch drop_option_blocks
        # leaves behind contains a literal `SAND_STEP_GATED(name, cond)`, and
        # strip_gated rewrites every occurrence of that macro into its second
        # argument - including the definition, which becomes `#define cond
        # (cond)` and so no longer matches the exact text removed here. That
        # artefact reached main once, where it renamed every later `cond`
        # identifier in sand_priv.h and in everything including it.
        text = strip_leftover_macros("\n".join(lines))

        text, n_gated = strip_gated(text)
        total["gated"] += n_gated

        lines, n_gate = strip_gate(text.split("\n"))
        total["gate"] += n_gate

        text = "\n".join(lines)

        if text != original:
            changed.append(rel)
            if not args.check:
                path.write_text(text, encoding="utf-8", newline="")

    # The build option itself, and the diagnostics default that turns it on.
    for rel, pattern in (("launcher/sdkconfig.defaults.diag", OPTION),):
        path = REPO / rel
        if path.exists():
            text = path.read_text(encoding="utf-8", newline="")
            kept = "\n".join(l for l in text.split("\n") if pattern not in l)
            if kept != text:
                changed.append(rel)
                if not args.check:
                    path.write_text(kept, encoding="utf-8", newline="")

    print("resolved %d lines of #if %s" % (total["blocks"], OPTION))
    print("unwrapped %d SAND_STEP_GATED expressions" % total["gated"])
    print("unwrapped %d SAND_STEP_GATE guards" % total["gate"])
    print("files changed: %d" % len(changed))
    for c in changed:
        print("  " + c)

    # Prose the object-code check cannot police - listed, never edited.
    stale = []
    for rel in SOURCES + ["launcher/main/Kconfig.projbuild"]:
        path = REPO / rel
        if not path.exists():
            continue
        for num, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
            if "SAND_STEP_GATE" in line or "sand_step_gate" in line or OPTION in line:
                stale.append("%s:%d:%s" % (rel, num, line.strip()[:88]))
    if stale:
        print("\nSTILL MENTIONING THE GATES - read these, they are comments and "
              "Kconfig, not code the verifier can check:")
        for s in stale:
            print("  " + s)

    if args.check and changed:
        sys.exit(1)


if __name__ == "__main__":
    main()
