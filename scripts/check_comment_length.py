#!/usr/bin/env python3
"""Report C/C++ comments longer than a character limit (default 300).

A run of consecutive own-line `//` comments, or of consecutive own-line
`/* */` blocks with no code between them, counts as ONE comment, the way a
reader sees it - otherwise one explanation chopped into several blocks would
each score under the limit while the paragraph they form does not. A `//`
trailing on a code line stands alone. Length is measured
on the comment's prose - markers, per-line indentation, `*` gutters and the
blank line inside a paragraph break are stripped first - so reformatting a
comment across more or fewer lines never changes its score.

Usage:
  check_comment_length.py [options] [<file>...]

Options:
  --limit N     characters a comment may not exceed (default 300)
  --top N       longest offenders to list (default 20; 0 for all)
  --files       list per-file counts instead of individual comments
  --all         include vendored and generated sources (excluded by default)
  --no-banners  ignore file/section header banners
  --changed REF check only files that differ from REF (the enforcement gate)
  --staged      check only files staged for commit
  --exit-zero   always exit 0, even with violations

  --comments-only REF   check nothing about length: assert instead that every
                        changed file differs from REF in COMMENTS ALONE. The
                        safety net for a bulk trim, delegated or not - a diff
                        of thousands of reflowed comments cannot be read, but
                        it can be proved to have moved no code.
"""

import os
import re
import subprocess
import sys

# Vendored upstream (microui, small3dlib) and machine-written headers: neither
# is ours to rewrite, and the generators' banner comments would dominate the
# report.
EXCLUDED = (
    "launcher/components/",
    "launcher/main/boot/boot_anim_curve.h",
    "launcher/main/boot/boot_anim_image.h",
    "launcher/main/boot/boot_anim_timeline.h",
    "launcher/main/gfx/fonts/font_lmroman_40.h",
)


class Comment:
    def __init__(self, path, line, kind, start=0, end=0):
        self.path = path
        self.line = line
        self.kind = kind  # "block" or "line"
        self.raw_lines = []
        self.spans = [(start, end)]

    @property
    def text(self):
        """The comment's prose: decoration stripped, paragraphs rejoined."""
        stripped = []
        for raw in self.raw_lines:
            s = raw.strip()
            if self.kind == "line":
                s = re.sub(r"^//+", "", s)
            else:
                s = re.sub(r"^/\*+", "", s)
                s = re.sub(r"\*+/$", "", s)
                s = re.sub(r"^\*+", "", s)
            stripped.append(s.strip())
        return " ".join(p for p in stripped if p)

    @property
    def length(self):
        return len(self.text)

    @property
    def line_range(self):
        return range(self.line, self.line + len(self.raw_lines))

    @property
    def has_rule(self):
        """Opens with a drawn rule - `/*====`, `//----`. A section header."""
        first = self.raw_lines[0].strip()
        return bool(re.match(r"^/\*[=*\-_#]{4,}", first)
                    or re.match(r"^//\s*[=*\-_#]{4,}", first))

    @property
    def is_banner(self):
        """A file or section header, not a comment sitting next to code.

        Only meaningful when the whole file was scanned: `line == 1` reads as
        "file header" here, but in an edit fragment line 1 is just wherever the
        fragment starts, which would exempt any comment written at its top.
        Code holding a fragment wants `has_rule`.
        """
        return self.line == 1 or self.has_rule


def scan(path, source):
    """Yield every comment in `source`, skipping string and char literals."""
    comments = []
    i, n = 0, len(source)
    line = 1
    while i < n:
        c = source[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in "\"'":
            quote = c
            i += 1
            while i < n and source[i] != quote:
                if source[i] == "\\":
                    i += 1
                elif source[i] == "\n":
                    line += 1
                i += 1
            i += 1
        elif source.startswith("/*", i):
            start_of_line = source.rfind("\n", 0, i) + 1
            own_line = source[start_of_line:i].strip() == ""
            end = source.find("*/", i + 2)
            end = n if end < 0 else end + 2
            prev = comments[-1] if comments else None
            mergeable = (
                own_line
                and prev is not None
                and prev.kind == "block"
                and prev.own_line
                and prev.line + len(prev.raw_lines) == line
            )
            if mergeable:
                prev.raw_lines += source[i:end].split("\n")
                prev.spans.append((i, end))
            else:
                com = Comment(path, line, "block", i, end)
                com.own_line = own_line
                com.raw_lines = source[i:end].split("\n")
                comments.append(com)
            line += source.count("\n", i, end)
            i = end
        elif source.startswith("//", i):
            start_of_line = source.rfind("\n", 0, i) + 1
            own_line = source[start_of_line:i].strip() == ""
            end = source.find("\n", i)
            end = n if end < 0 else end
            prev = comments[-1] if comments else None
            mergeable = (
                own_line
                and prev is not None
                and prev.kind == "line"
                and prev.own_line
                and prev.line + len(prev.raw_lines) == line
            )
            if mergeable:
                prev.raw_lines.append(source[i:end])
                prev.spans.append((i, end))
            else:
                com = Comment(path, line, "line", i, end)
                com.own_line = own_line
                com.raw_lines = [source[i:end]]
                comments.append(com)
            i = end
        else:
            i += 1
    return comments


def tracked_sources():
    out = subprocess.run(
        ["git", "ls-files", "*.c", "*.h", "*.cpp", "*.hpp"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return [p for p in out.splitlines() if p]


def code_only(source):
    """The source with every comment replaced by a space, whitespace flattened.

    Two files that agree here differ in nothing but comments and layout, which
    is what makes a bulk comment rewrite reviewable at all: the diff is large
    by nature, so the guarantee has to come from a check rather than a read.
    """
    out, at = [], 0
    for com in scan("", source):
        for start, end in com.spans:
            out.append(source[at:start])
            out.append(" ")
            at = end
    out.append(source[at:])
    return re.sub(r"\s+", " ", "".join(out)).strip()


def changed_files(ref):
    """Tracked C/C++ files that differ from `ref`, staged or not."""
    args = ["git", "diff", "--name-only", "--diff-filter=ACMR"]
    out = subprocess.run(args + [ref] if ref else args + ["--cached"],
                         capture_output=True, text=True).stdout
    exts = (".c", ".h", ".cpp", ".hpp")
    seen = [p for p in dict.fromkeys(out.split("\n")) if p.endswith(exts)]
    return [p for p in seen if os.path.exists(p)]


def added_lines(ref, path):
    """Line numbers this change adds or rewrites, in the file as it now is.

    The gate scopes itself to these so a 2000-violation backlog does not make
    every unrelated edit fail: touching a file is not the same as endorsing
    every comment already in it.
    """
    args = ["git", "diff", "-U0", "--diff-filter=ACMR"]
    args += [ref] if ref else ["--cached"]
    out = subprocess.run(args + ["--", path],
                         capture_output=True, text=True).stdout
    lines = set()
    for hunk in re.finditer(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", out, re.M):
        start = int(hunk.group(1))
        count = int(hunk.group(2) or 1)
        lines.update(range(start, start + count))
    return lines


def file_at_ref(ref, path):
    # text=True alone decodes with the platform default (cp1252 on
    # Windows), which mangles any non-ASCII byte a source file carries (an
    # em dash, say) into extra characters - harmless for ASCII-only files,
    # but it makes --comments-only misreport a real code change on one
    # that isn't. Source files are UTF-8; decode them as such.
    r = subprocess.run(["git", "show", f"{ref}:{path}"],
                       capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return None if r.returncode else r.stdout


def verify_comments_only(ref, paths):
    """Fail if any changed file differs from `ref` in anything but comments."""
    paths = paths or changed_files(ref)
    bad = []
    for path in paths:
        before = file_at_ref(ref, path)
        if before is None:
            print(f"new file, nothing to compare: {path}")
            continue
        after = open(path, encoding="utf-8", errors="replace").read()
        if code_only(before) != code_only(after):
            bad.append(path)
    for path in bad:
        print(f"CODE CHANGED: {path}")
    print()
    print(f"compared against {ref}: {len(paths)} file"
          f"{'' if len(paths) == 1 else 's'}, "
          f"{len(bad)} with code changes")
    return 1 if bad else 0


def relative_to_root(path):
    """Report paths the way the repo names them, so they stay clickable."""
    try:
        rel = os.path.relpath(path)
    except ValueError:  # another drive on Windows
        return path
    return path if rel.startswith("..") else rel.replace(os.sep, "/")


def main(argv):
    limit, top, per_file, include_all, exit_zero = 300, 20, False, False, False
    no_banners = False
    changed_ref, staged, comments_only_ref = None, False, None
    paths = []
    it = iter(argv)
    for arg in it:
        if arg == "--limit":
            limit = int(next(it))
        elif arg == "--top":
            top = int(next(it))
        elif arg == "--files":
            per_file = True
        elif arg == "--all":
            include_all = True
        elif arg == "--exit-zero":
            exit_zero = True
        elif arg == "--no-banners":
            no_banners = True
        elif arg == "--changed":
            changed_ref = next(it)
        elif arg == "--staged":
            changed_ref = ""
            staged = True
        elif arg == "--comments-only":
            comments_only_ref = next(it)
        elif arg in ("-h", "--help"):
            print(__doc__)
            return 0
        elif arg.startswith("-"):
            print(f"unknown option: {arg}", file=sys.stderr)
            return 2
        else:
            paths.append(arg)

    paths = [relative_to_root(p) for p in paths]
    explicit = bool(paths)

    if comments_only_ref is not None:
        return verify_comments_only(comments_only_ref, paths)

    if explicit:
        pass
    elif changed_ref is not None or staged:
        paths = changed_files(changed_ref or "")
        if not paths:
            print("no changed C/C++ files")
            return 0
    else:
        paths = tracked_sources()

    if not include_all and not explicit:
        paths = [p for p in paths if not any(p.startswith(x) for x in EXCLUDED)]

    scoped = (changed_ref is not None or staged) and not explicit
    comments = []
    for path in paths:
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                found = scan(path, f.read())
        except OSError as e:
            print(f"skipped {path}: {e}", file=sys.stderr)
            continue
        if scoped:
            touched = added_lines(changed_ref or "", path)
            found = [c for c in found if touched & set(c.line_range)]
        comments += found

    if no_banners:
        comments = [c for c in comments if not c.is_banner]

    over = sorted([c for c in comments if c.length > limit],
                  key=lambda c: -c.length)

    if per_file:
        by_file = {}
        for c in over:
            by_file.setdefault(c.path, []).append(c)
        for path in sorted(by_file, key=lambda p: -len(by_file[p])):
            worst = max(x.length for x in by_file[path])
            print(f"{len(by_file[path]):4d}  (worst {worst:5d})  {path}")
    else:
        for c in over[: top or None]:
            print(f"{c.path}:{c.line}: {c.length} chars")
            print(f"      {c.text[:100]}...")
        if top and len(over) > top:
            print(f"... and {len(over) - top} more (--top 0 for all)")

    total = len(comments)
    pct = (100.0 * len(over) / total) if total else 0.0
    files_over = len({c.path for c in over})
    print()
    print(f"limit           {limit} characters")
    print(f"files scanned   {len(paths)}")
    print(f"comments        {total}")
    print(f"over the limit  {len(over)}  ({pct:.1f}%)"
          f"  in {files_over} file{'' if files_over == 1 else 's'}")
    if over:
        lengths = sorted(c.length for c in over)
        banners = sum(1 for c in over if c.is_banner)
        print(f"longest         {lengths[-1]} characters"
              f"  (median offender {lengths[len(lengths) // 2]})")
        print(f"  of those       {banners} file/section header banners,"
              f" {len(over) - banners} beside code")
        print()
        print("were the limit instead:")
        for alt in (200, 300, 400, 600, 800, 1200):
            n = sum(1 for c in comments if c.length > alt)
            print(f"  {alt:5d}  {n:5d} over  ({100.0 * n / total:5.1f}%)")

    return 0 if (exit_zero or not over) else 1


if __name__ == "__main__":
    # Defaults come from `git ls-files`, so run from the repo root - but the
    # caller's own arguments were written relative to wherever they stood.
    args = [os.path.abspath(a) if os.path.exists(a) else a
            for a in sys.argv[1:]]
    os.chdir(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )
    sys.exit(main(args))
