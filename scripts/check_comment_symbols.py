#!/usr/bin/env python3
"""Fail on a comment naming a function that does not exist.

    python scripts/check_comment_symbols.py [root]

A trim that garbles a cited name leaves a comment pointing at nothing, which
is worse than the long comment it replaced. Anything written as `name()` in a
comment must appear as a declaration or a call somewhere under `root`.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_comment_length import EXCLUDED, scan  # noqa: E402

SKIP = ("build", "build.dev", "build.diag", "managed_components")
CITED = re.compile(r"\b([a-z_][a-z0-9_]{4,})\(\)")
DEFINED = re.compile(r"\b([a-z_][a-z0-9_]{4,})\s*\(")

# Named in comments as the C library or the vendor SDK spells them, with no
# definition in this tree to find.
FOREIGN = {"main", "printf", "malloc", "free", "memset", "memcpy", "assert"}


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "launcher"
    sources, cited = {}, {}
    for p in sorted(pathlib.Path(root).rglob("*")):
        if p.suffix not in (".c", ".h") or any(s in p.parts for s in SKIP):
            continue
        rp = p.as_posix()
        sources[rp] = p.read_text(encoding="utf-8", errors="replace")
        if any(rp.startswith(e) for e in EXCLUDED):
            continue
        for c in scan(rp, sources[rp]):
            for name in CITED.findall(c.text):
                if name not in FOREIGN:
                    cited.setdefault(name, set()).add(f"{rp}:{c.line}")

    defined = set(DEFINED.findall("\n".join(sources.values())))
    missing = {n: v for n, v in cited.items() if n not in defined}
    for name, where in sorted(missing.items()):
        for site in sorted(where):
            print(f"{site}: comment names {name}(), which does not exist")
    print(f"{len(cited)} symbols cited in comments, {len(missing)} missing")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
