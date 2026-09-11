#!/usr/bin/env python3
"""Rewrite drawn-rule comments as plain style(9) blocks, keeping the prose.

    python scripts/strip_comment_rules.py [--check] [root]

A `/*====` or `/*----` rule is decoration: OpenBSD style(9) has three comment
shapes and none of them draws one. Every word of prose survives; only the rule
lines and the leading and trailing rule runs go. A comment that was nothing but
rule is deleted outright.

The rewrite is proved rather than read: each file must agree with its original
under check_comment_length.code_only(), and each comment's prose must come back
character for character.
"""
import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_comment_length import EXCLUDED, code_only, scan  # noqa: E402

SKIP = ("managed_components", "build", "build.dev", "build.diag")
ONE_LINE_MAX = 78

LEAD_RULE = re.compile(r"^[=*_#\-]{4,}")
TAIL_RULE = re.compile(r"[=*_#\-]{4,}$")


def restyle(span, indent):
    """One `/* ... */` span as a style(9) block, or None if it was all rule."""
    inner = span[2:-2] if span.endswith("*/") else span[2:]
    prose = []
    for k, raw in enumerate(inner.split("\n")):
        # Only the gutter comes off, so indentation a comment uses to lay out
        # a quoted expression survives the rewrite.
        s = raw.strip() if k == 0 else re.sub(r"^\s*\*+ ?", "", raw).rstrip()
        if k and s == raw.rstrip():
            s = raw.strip()
        bare = TAIL_RULE.sub("", LEAD_RULE.sub("", s.strip())).strip()
        prose.append(bare if bare != s.strip() else s)
    while prose and not prose[0]:
        prose.pop(0)
    while prose and not prose[-1]:
        prose.pop()
    if not prose:
        return None
    if len(prose) == 1 and len(indent) + len(prose[0]) + 6 <= ONE_LINE_MAX:
        return indent + "/* " + prose[0] + " */"
    out = [indent + "/*"]
    out += [(indent + " * " + s).rstrip() for s in prose]
    out.append(indent + " */")
    return "\n".join(out)


def rewrite(path, source):
    """The source with every drawn rule gone, or None if it carried none."""
    edits = []
    for c in scan(path, source):
        if not c.has_rule:
            continue
        for start, end in c.spans:
            indent = source[source.rfind("\n", 0, start) + 1:start]
            if indent.strip():
                continue
            block = restyle(source[start:end], indent)
            if block is None:
                eol = source.find("\n", end)
                eol = len(source) if eol < 0 else eol
                drop = eol + 1 if not source[end:eol].strip() else end
                edits.append((start - len(indent), drop, ""))
            else:
                edits.append((start - len(indent), end, block))
    if not edits:
        return None
    out, at = [], 0
    for start, end, block in edits:
        out.append(source[at:start])
        out.append(block)
        at = end
    out.append(source[at:])
    return "".join(out)


RULE_WORD = re.compile(r"^[=*_#\-]{4,}$")


def words(text):
    """The words of a comment with drawn rule removed, so the same text reads
    the same before and after the rewrite."""
    out = []
    for w in text.split():
        w = TAIL_RULE.sub("", LEAD_RULE.sub("", w))
        if w and not RULE_WORD.match(w):
            out.append(w)
    return out


def prose_of(path, source):
    return [words(c.text) for c in scan(path, source)]


def sources(root):
    for p in sorted(pathlib.Path(root).rglob("*")):
        if p.suffix not in (".c", ".h") or any(s in p.parts for s in SKIP):
            continue
        if any(p.as_posix().startswith(e) for e in EXCLUDED):
            continue
        yield p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", default="launcher")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    touched = rules = 0
    for p in sources(args.root):
        src = p.read_text(encoding="utf-8")
        new = rewrite(p.as_posix(), src)
        if new is None or new == src:
            continue
        if code_only(new) != code_only(src):
            print(f"REFUSED (code moved): {p}", file=sys.stderr)
            return 1
        before = [w for t in prose_of(p.as_posix(), src) for w in t]
        after = [w for t in prose_of(p.as_posix(), new) for w in t]
        if before != after:
            print(f"REFUSED (prose changed): {p}", file=sys.stderr)
            for a, b in zip(before, after):
                if a != b:
                    print(f"  {a!r} -> {b!r}", file=sys.stderr)
                    break
            return 1
        rules += sum(1 for c in scan(p.as_posix(), src) if c.has_rule)
        touched += 1
        if not args.check:
            p.write_text(new, encoding="utf-8", newline="\n")

    verb = "would lose" if args.check else "lost"
    print(f"{touched} files {verb} {rules} drawn rules")
    return 0


if __name__ == "__main__":
    sys.exit(main())
