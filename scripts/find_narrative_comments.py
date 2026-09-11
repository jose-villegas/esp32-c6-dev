#!/usr/bin/env python3
"""List comments that narrate how the code got here, worst first.

    python scripts/find_narrative_comments.py [--min-chars 300] [--json PATH]

CLAUDE.md's first comment rule: a comment states the constraint that holds
now, never the journey. This finds candidates for that rule by keyword, so it
is a WORKLIST, NOT A VERDICT - "previously" can describe current behaviour and
"failed" can be what a test asserts. Every hit still needs a person deciding
whether what is left is a constraint or a story.
"""
import argparse
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_comment_length import scan  # noqa: E402

SIGNS = re.compile(
    r"(a first attempt|an earlier version|was considered|used to |reverted|"
    r"before this|previously|the old |at first|turned out|tried |attempt|"
    r"originally|no longer|once (?:did|was|let|made)|instead of the|"
    r"we (?:tried|found|had)|did not work|failed|regressed|"
    r"this (?:used|was) |earlier|first version|second version|"
    r"now that|since the fix|after the fix)", re.I)

SKIP = ("managed_components", "components", "build", "build.dev", "build.diag")


def find(root, min_chars):
    for p in sorted(pathlib.Path(root).rglob("*")):
        if p.suffix not in (".c", ".h") or any(s in p.parts for s in SKIP):
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        for c in scan(p.as_posix(), text):
            if c.length > min_chars and SIGNS.search(c.text):
                yield {"chars": c.length, "lines": c.lines,
                       "path": p.as_posix(), "line": c.line,
                       "banner": c.is_banner}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", default="launcher")
    ap.add_argument("--min-chars", type=int, default=300)
    ap.add_argument("--json")
    args = ap.parse_args()

    rows = sorted(find(args.root, args.min_chars), key=lambda r: -r["chars"])
    for r in rows[:25]:
        print(f"  {r['chars']:5d} chars {r['lines']:4d} lines  "
              f"{r['path']}:{r['line']}")
    files = {r["path"] for r in rows}
    print(f"\n{len(rows)} comments, {sum(r['chars'] for r in rows):,} chars, "
          f"{len(files)} files")
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(rows, indent=1),
                                           encoding="utf-8")
        print(f"worklist -> {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
