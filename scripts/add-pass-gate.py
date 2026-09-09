#!/usr/bin/env python
"""Add one sand pass gate to the tree - the three mechanical edits, not the
judgement call.

    python scripts/add-pass-gate.py --name gas_rise [--comment "..."]

The inverse of scripts/strip-pass-gates.py, and its round trip is the test:
add a couple of gates, strip them, and `git diff --exit-code` must come back
clean. Everything this script writes is shaped so that deleting exactly the
`#if CONFIG_LAUNCHER_SAND_PASS_GATES ... #endif` lines - which is all
strip-pass-gates.py does - restores the file byte for byte. That is why a
block is inserted immediately after its anchor line and never with a blank
line of its own: a blank line the strip does not own would survive it.

WHAT IT DOES: the `extern volatile bool` in sand_priv.h, the definition in
sand.c, and CONFIG_LAUNCHER_SAND_PASS_GATES=y in
launcher/sdkconfig.defaults.diag. Between rounds the tree carries no gates at
all, so the first call also recreates the whole #if block, the two macros and
their #else no-op forms included.

WHAT IT DELIBERATELY DOES NOT DO: wrap the call site. WHERE a gate goes is
the entire content of a decomposition - a gate around the wrong brace measures
a different question and still compiles, still runs, and still prints a
plausible number. So the script prints the two macro forms and stops. See
docs/sand/Perf-Round-Guide.md, "Instrumenting a round".

It also does not commit. The gates are scaffolding: the round's convention is
a commit marked TEMP or PROBE that is stripped before the PR, and choosing
that boundary is the human's.

WHERE THE DEFINITIONS GO, and why not next to sand_step(): a round once put
them between `__attribute__((aligned(32)))` and the function that attribute
was written for, which silently moved the attribute onto a bool - in a
campaign where function alignment is one of the things being measured. The
top of sand.c, right after the includes, cannot decorate anything.
"""
import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

PRIV = REPO / "launcher/main/apps/sand/sand_priv.h"
DEFS = REPO / "launcher/main/apps/sand/sand.c"
DIAG = REPO / "launcher/sdkconfig.defaults.diag"

OPTION = "CONFIG_LAUNCHER_SAND_PASS_GATES"
IF_LINE = "#if " + OPTION
ENDIF = "#endif"

MACRO_GATE = "#define SAND_STEP_GATE(name)        if (sand_step_gate_##name)"
MACRO_GATED = "#define SAND_STEP_GATED(name, cond) (sand_step_gate_##name && (cond))"
NOOP_GATE = "#define SAND_STEP_GATE(name)"
NOOP_GATED = "#define SAND_STEP_GATED(name, cond) (cond)"

# The prose already in sand_priv.h that says where the gates belong. It is
# what strip-pass-gates.py leaves behind on purpose (it removes code, never
# comments), so between rounds it is the only marker left - which makes it
# this script's anchor rather than a line number that rots.
PRIV_ANCHOR_MARKERS = ("Per-pass volatile gates", "SAND_PASS_GATES")

BLOCK_HEADER = [
    "/* SCAFFOLDING for one round, removed at the end of it by",
    " * scripts/strip-pass-gates.py. Volatile is load-bearing: an #if would let",
    " * the compiler prove the guarded work unreachable and delete the walk that",
    " * reaches it, which is how an earlier code-skip probe measured nothing. */",
]

DEFS_HEADER = [
    "/* Gate definitions, kept at the top of the file rather than beside",
    " * sand_step(): a definition dropped between an attribute and the function",
    " * it was written for silently steals the attribute, and this campaign",
    " * measures function alignment. */",
]

NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")


def read_lines(path):
    """Line list with the file's own newlines preserved (this tree is LF, but
    the strip script is newline-agnostic and so is this one)."""
    text = path.read_text(encoding="utf-8", newline="")
    eol = "\r\n" if "\r\n" in text else "\n"
    return text.split(eol), eol


def write_lines(path, lines, eol):
    path.write_text(eol.join(lines), encoding="utf-8", newline="")


def find_block(lines):
    """(start, end) line indices of the #if OPTION ... #endif block, or None.

    Only the top-level block is looked for: gate blocks never nest, and a
    nested #if inside one would be a different feature's, not a gate's."""
    for i, line in enumerate(lines):
        if line.strip().startswith(IF_LINE):
            depth = 0
            for j in range(i + 1, len(lines)):
                s = lines[j].strip()
                if s.startswith("#if"):
                    depth += 1
                elif s.startswith("#endif"):
                    if depth == 0:
                        return i, j
                    depth -= 1
            raise SystemExit("unterminated %s in the tree" % IF_LINE)
    return None


def comment_blocks_from(lines, start):
    """Yield (first, last, text) for each /* */ block at or after `start`,
    skipping blank lines, stopping at the first line that is neither."""
    i = start
    while i < len(lines):
        s = lines[i].strip()
        if not s:
            i += 1
            continue
        if not s.startswith("/*"):
            return
        j = i
        while j < len(lines) and "*/" not in lines[j]:
            j += 1
        if j >= len(lines):
            return
        yield i, j, " ".join(lines[i:j + 1])
        i = j + 1


def priv_anchor(lines):
    """Index of the last line of the run of gate prose in sand_priv.h.

    Loud rather than clever when it is missing: guessing an insertion point
    in a 1000-line header is how a declaration lands inside somebody else's
    #ifdef and the build breaks somewhere unrelated."""
    first = None
    for i, line in enumerate(lines):
        if PRIV_ANCHOR_MARKERS[0] in line:
            first = i
            break
    if first is None:
        raise SystemExit(
            "sand_priv.h no longer carries the '%s' comment this script anchors "
            "on. Restore it, or add the block by hand - see "
            "docs/sand/Perf-Round-Guide.md, 'The instrument'."
            % PRIV_ANCHOR_MARKERS[0])

    last = None
    for start, end, text in comment_blocks_from(lines, first):
        if not any(m in text for m in PRIV_ANCHOR_MARKERS):
            break
        last = end
    return last


def defs_anchor(lines):
    """The blank line after sand.c's top-of-file include block. Insert after
    it and the definitions read as their own paragraph while still being
    deletable as exactly the #if..#endif lines."""
    last_include = None
    for i, line in enumerate(lines[:80]):
        if line.startswith("#include"):
            last_include = i
    if last_include is None:
        raise SystemExit("no #include found at the top of sand.c")
    j = last_include + 1
    if j >= len(lines) or lines[j].strip():
        raise SystemExit("expected a blank line after sand.c's includes")
    return j


def build_priv_block(name, comment):
    body = list(BLOCK_HEADER)
    if comment:
        body += wrap_comment(comment)
    body.append("extern volatile bool sand_step_gate_%s;" % name)
    return [IF_LINE] + body + [
        "",
        MACRO_GATE,
        MACRO_GATED,
        "#else",
        NOOP_GATE,
        NOOP_GATED,
        ENDIF,
    ]


def wrap_comment(comment, width=74):
    """A one-line /* */ if it fits, a wrapped block if it does not. The repo's
    comment-length hook scores prose, so this stays out of its way by simply
    not adding any of its own."""
    text = " ".join(comment.split())
    if len(text) + 6 <= width:
        return ["/* %s */" % text]
    out, line = [], "/*"
    for word in text.split():
        candidate = (line + " " + word) if line != "/*" else "/* " + word
        if len(candidate) > width:
            out.append(line)
            line = " * " + word
        else:
            line = candidate
    out.append(line + " */")
    return out


def add_to_priv(name, comment):
    lines, eol = read_lines(PRIV)
    decl = "extern volatile bool sand_step_gate_%s;" % name
    if any(line.strip() == decl for line in lines):
        return False

    block = find_block(lines)
    if block is None:
        at = priv_anchor(lines)
        lines[at + 1:at + 1] = build_priv_block(name, comment)
    else:
        start, _end = block
        # Just above the blank line that separates the externs from the two
        # macros, so the declarations stay one list however many rounds add
        # to it.
        insert_at = None
        for j in range(start + 1, _end):
            if lines[j].startswith("#define SAND_STEP_GATE"):
                insert_at = j - 1 if not lines[j - 1].strip() else j
                break
        if insert_at is None:
            insert_at = _end
        new = (wrap_comment(comment) if comment else []) + [decl]
        lines[insert_at:insert_at] = new
    write_lines(PRIV, lines, eol)
    return True


def add_to_defs(name):
    lines, eol = read_lines(DEFS)
    define = "volatile bool sand_step_gate_%s = true;" % name
    if any(line.strip() == define for line in lines):
        return False

    block = find_block(lines)
    if block is None:
        at = defs_anchor(lines)
        lines[at + 1:at + 1] = [IF_LINE] + DEFS_HEADER + [define, ENDIF]
    else:
        _start, end = block
        lines[end:end] = [define]
    write_lines(DEFS, lines, eol)
    return True


def add_to_diag():
    lines, eol = read_lines(DIAG)
    if any(line.strip().startswith(OPTION) for line in lines):
        return False
    # Appended as the last line of the file, which is exactly what
    # strip-pass-gates.py drops again - it filters out every line mentioning
    # the option and rejoins, so a trailing newline is preserved either way.
    at = len(lines) - 1 if lines and lines[-1] == "" else len(lines)
    lines[at:at] = [OPTION + "=y"]
    write_lines(DIAG, lines, eol)
    return True


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True,
                    help="gate name, without the sand_step_gate_ prefix")
    ap.add_argument("--comment", default=None,
                    help="one line of WHY, placed above the declaration")
    args = ap.parse_args()

    name = args.name
    if name.startswith("sand_step_gate_"):
        name = name[len("sand_step_gate_"):]
    if not NAME_RE.match(name):
        raise SystemExit("gate names are lower_snake_case: %r" % args.name)

    did = {
        "sand_priv.h": add_to_priv(name, args.comment),
        "sand.c": add_to_defs(name),
        "sdkconfig.defaults.diag": add_to_diag(),
    }
    for where, changed in did.items():
        print("%-24s %s" % (where, "added" if changed else "already there"))

    print()
    print("Now place the call site yourself - which brace the gate goes around")
    print("IS the question being measured, so this script will not guess:")
    print()
    print("    SAND_STEP_GATE(%s) { ... }              /* a call or a block */" % name)
    print("    SAND_STEP_GATED(%s, existing_cond)      /* an existing condition */" % name)
    print()
    print("Then take the capture perf-scoped (there is no static RAM for a new")
    print("decomposition row otherwise), and strip everything at the end of the")
    print("round with scripts/strip-pass-gates.py.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
