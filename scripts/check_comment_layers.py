#!/usr/bin/env python3
"""Fail on a comment below `apps/` that names a particular app.

    python scripts/check_comment_layers.py [--context]

An app is a folder designed to be deleted whole, so a comment in a lower layer
naming one is a dangling reference by construction: delete the app and the
comment survives the code it described. Say what shape of caller needs the
thing, or state the rule a caller must follow. An app's own files may name
anything below them - that direction cannot dangle.

App names come from the folders themselves, so adding an app extends the check.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_comment_length import EXCLUDED, scan  # noqa: E402

LOWER = ("launcher/main/boot/", "launcher/main/display/", "launcher/main/gfx/",
         "launcher/main/input/", "launcher/main/ui/", "launcher/main/util/",
         "launcher/test/")
SKIP = ("build", "build.dev", "build.diag", "managed_components")

# The shell's own two files: they switch between apps without knowing one.
SHELL = ("launcher/main/app.h", "launcher/main/main.c")

# "the diagnostics build" is the build variant behind build.diag, which every
# layer may name; the app that happens to share the word is what this forbids.
VARIANT = re.compile(r"\bdiagnostics builds?\b", re.I)


def app_names(root="launcher/main/apps"):
    return sorted(p.name for p in pathlib.Path(root).iterdir() if p.is_dir())


def sources():
    for p in sorted(pathlib.Path("launcher").rglob("*")):
        if p.suffix not in (".c", ".h") or any(s in p.parts for s in SKIP):
            continue
        rp = p.as_posix()
        if any(rp.startswith(e) for e in EXCLUDED):
            continue
        if rp.startswith(LOWER) or rp in SHELL:
            yield rp


def main():
    context = "--context" in sys.argv[1:]
    names = app_names()
    word = re.compile(r"\b(" + "|".join(names) + r")\b", re.I)
    found = 0
    for rp in sources():
        text = pathlib.Path(rp).read_text(encoding="utf-8", errors="replace")
        for c in scan(rp, text):
            hits = sorted(set(m.lower()
                              for m in word.findall(VARIANT.sub("", c.text))))
            if not hits:
                continue
            found += 1
            print(f"{rp}:{c.line}: comment names {', '.join(hits)}")
            if context:
                print(f"      {c.text[:160]}")
    print(f"{found} comment{'' if found == 1 else 's'} below apps/ "
          f"name an app ({', '.join(names)})")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
