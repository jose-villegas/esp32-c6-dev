#!/usr/bin/env python3
"""Shorten over-long C comments with a local Ollama model, one comment at a time.

The model is handed a comment's PROSE and nothing else - never a line of code,
never the file. What comes back is re-wrapped into the original comment's own
shape and dropped into its original span. Code cannot be damaged by a bad
generation because code is never in the model's hands; the worst case is a bad
sentence, which the report puts in front of a reviewer.

A comment the model cannot get under `--ceiling` in `--retries` attempts keeps
its ORIGINAL text. Failing to shorten is a fine outcome; silently losing a
constraint is not. One that lands between `--limit` and `--ceiling` is kept
as an improvement even though it missed the aim - a comment that truly needs
the room is allowed up to the ceiling, it just should not still be at its
starting length.

A comment that is pure change-history narration - what it used to do, when it
was fixed, an earlier version's behaviour - with no constraint left that
still applies is DELETED entirely rather than shortened: git log already owns
that history, and every deletion is flagged in both reports for a human to
confirm nothing load-bearing went with it.

Usage:
  trim_comments_local.py [options] [<path>...]     (default: the sand app)

Options:
  --limit N       character aim, retried against (default 300)
  --ceiling N     hard cap - a result over this is left unresolved, original
                  text kept (default 500)
  --model NAME    ollama model (default qwen2.5-coder:32b-instruct-q4_K_M)
  --retries N     attempts per comment before giving up (default 3)
  --banners       also rewrite file/section header banners (off: their `====`
                  rules do not survive re-wrapping)
  --max N         stop after N comments, for a smoke test
  --dry-run       write nothing; still produce the report
  --report PATH   markdown report (default scripts/results/comment-trim.md)

Reviewing a finished run - it writes its was/now pairs to
scripts/results/comment-trim.json, and the review reads that back, so it can
run later, on another machine, or with a different reviewer:

  --review        check each rewrite for dropped numbers, dropped named
                  functions/files/macros and dropped negations (no model
                  needed for any of that), then ask --review-model for a
                  verdict on meaning. Writes comment-trim-review.md, flagged
                  first.
  --review-model NAME   reviewer (default mistral-nemo:latest; `none` for the
                  deterministic checks alone)
  --review-packet DIR   instead, write the pairs out as prose-only chunks for
                  a reviewer this script cannot call - a stronger model, or a
                  person. No code goes in them.
  --pairs PATH    the pairs file to review (default the one above)
"""

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_comment_length import code_only, scan  # noqa: E402

DEFAULT_MODEL = "qwen2.5-coder:32b-instruct-q4_K_M"
DEFAULT_REVIEW_MODEL = "mistral-nemo:latest"
DEFAULT_PATHS = ["launcher/main/apps/sand"]

PROMPT = """You shorten source-code comments. Rewrite the comment below so it \
is at most {limit} characters, and reply with NOTHING but the rewritten \
prose - OR, if the whole comment is only a change-history narration (what \
it used to do, what it was changed to, when a bug was fixed) with no \
constraint or reason that still applies to the code as it stands today, \
reply with exactly the single word DELETE. git log already owns that \
history; the comment does not need to repeat it.

Rules:
- Keep every concrete fact: numbers with units, measured percentages and frame
  rates, hardware constraints, named functions and files, and any alternative
  that was tried and rejected together with the reason it was rejected.
- Do not re-attribute a reason. If the text says X is done because of A, your
  version must not say it is done because of B.
- If the text describes a LIMITATION or a problem, your version must still read
  as a limitation, not as a benefit.
- Cut: narration of how the code got here over time (first attempt, second
  attempt, this version), restatements of what the code obviously does, and
  repeated phrasing.
- British spelling as in the original (colour, behaviour). Plain prose, no
  bullet lists, no headings, no markdown, no code fences, no preamble.

Comment ({length} characters):
{text}
"""

RETRY = """That was {got} characters, still over {limit}. Rewrite it shorter. \
Reply with nothing but the prose.

{text}
"""


def ask(model, prompt, log):
    started = time.time()
    with open(log, "a", encoding="utf-8") as f:
        f.write(f"\n===== {time.strftime('%H:%M:%S')} =====\n{prompt}\n")
    r = subprocess.run(
        ["ollama", "run", model, "--think=false", "--nowordwrap"],
        input=prompt, capture_output=True, text=True, encoding="utf-8",
        errors="replace",
    )
    out = clean(r.stdout or "")
    with open(log, "a", encoding="utf-8") as f:
        f.write(f"----- {time.time() - started:.1f}s -----\n{out}\n")
    return out


def clean(raw):
    """Strip the wrapper a chat model puts around the thing you asked for."""
    text = re.sub(r"```[a-z]*\n?", "", raw)
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.S)
    lines = [ln for ln in text.strip().split("\n")]
    while lines and re.match(r"^(here|sure|okay|certainly|rewritten|shortened)"
                             r"\b.*:\s*$", lines[0].strip(), re.I):
        lines.pop(0)
    return re.sub(r"\s+", " ", " ".join(lines)).strip()


LEADING_DELETE = re.compile(r"^\s*DELETE\b", re.I)
TRAILING_DELETE = re.compile(r"[:.\s]*\bDELETE\s*\.?\s*$", re.I)


def split_delete(prose):
    """A model asked to reply with prose OR the bare word DELETE sometimes
    mixes the two, two different ways that need opposite handling:

    - DELETE leading, with or without a reason after it ("DELETE: this is
      just change history") - a real delete decision, reason discarded.
    - DELETE trailing, tacked onto an otherwise complete rewrite - a leftover
      artifact of the model trying to do both; the rewrite is the real
      answer and the stray word must not end up written into a live comment.
    """
    if LEADING_DELETE.match(prose):
        return True, ""
    return False, TRAILING_DELETE.sub("", prose).strip() or prose


def rewrap(comment, prose, width, source):
    """Put `prose` back into the shape the original comment had.

    The indentation has to come from `source`: a comment's span starts at its
    marker, so the whitespace in front of it is never part of the comment.
    """
    start = comment.spans[0][0]
    bol = source.rfind("\n", 0, start) + 1
    head = source[bol:start]
    # A comment trailing code aligns its continuation lines under itself.
    indent = head if head.strip() == "" else " " * len(head)
    if comment.kind == "line":
        body, lead = "// ", indent + "// "
    else:
        body, lead = " * ", indent + " * "

    words, lines, cur = prose.split(), [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > width - len(lead):
            lines.append(cur)
            cur = w
        else:
            cur = f"{cur} {w}".strip()
    if cur:
        lines.append(cur)

    # The span starts AT the marker, so the first line's own indentation is
    # already in the file and must not be written again.
    if comment.kind == "line":
        out = [body + lines[0]] + [lead + ln for ln in lines[1:]]
        return "\n".join(out)
    out = ["/* " + lines[0]] + [lead + ln for ln in lines[1:]]
    out[-1] += " */"
    return "\n".join(out)


def wrap_width(source):
    widths = [len(ln) for ln in source.split("\n") if ln.strip()]
    return min(78, max(widths) if widths else 78)


def whole_line_span(comment, source):
    """The comment's own lines, leading indentation and trailing newline
    included, so removing it leaves no blank line behind."""
    start = comment.spans[0][0]
    end = comment.spans[-1][1]
    bol = source.rfind("\n", 0, start) + 1
    eol = source.find("\n", end)
    eol = len(source) if eol < 0 else eol + 1
    return bol, eol


def trim_file(path, opts, log, results):
    source = open(path, encoding="utf-8", errors="replace").read()
    width = wrap_width(source)
    targets = [c for c in scan(path, source) if c.length > opts["limit"]]
    if not opts["banners"]:
        targets = [c for c in targets if not c.is_banner]
    if not targets:
        return 0

    edits = []
    for com in targets:
        if opts["max"] and results["attempted"] >= opts["max"]:
            break
        results["attempted"] += 1
        original = com.text
        prose = ask(opts["model"],
                    PROMPT.format(limit=opts["limit"], length=com.length,
                                  text=original), log)
        wants_delete, prose = split_delete(prose)

        if com.own_line and wants_delete:
            row = {"path": path, "line": com.line, "before": com.length,
                   "after": 0, "tries": 1, "kept": False, "deleted": True,
                   "original": original, "new": ""}
            results["trimmed"].append(row)
            start, end = whole_line_span(com, source)
            edits.append((start, end, ""))
            continue

        tries = 1
        while prose and len(prose) > opts["limit"] and tries < opts["retries"]:
            prose = ask(opts["model"],
                        RETRY.format(got=len(prose), limit=opts["limit"],
                                     text=prose), log)
            tries += 1

        row = {"path": path, "line": com.line, "before": com.length,
               "after": len(prose) if prose else 0, "tries": tries,
               "original": original, "new": prose}
        if not prose or len(prose) > opts["ceiling"]:
            row["kept"] = True
            results["unresolved"].append(row)
            continue
        row["kept"] = False
        row["over_aim"] = len(prose) > opts["limit"]
        results["trimmed"].append(row)
        start = com.spans[0][0]
        end = com.spans[-1][1]
        edits.append((start, end, rewrap(com, prose, width, source)))

    if not edits:
        return 0

    out = source
    for start, end, text in sorted(edits, reverse=True):
        out = out[:start] + text + out[end:]

    if code_only(source) != code_only(out):
        results["rejected"].append(path)
        return 0
    if not opts["dry_run"]:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(out)
    return len(edits)


def sources_under(paths):
    found = []
    for p in paths:
        if os.path.isfile(p):
            found.append(p)
            continue
        for root, dirs, files in os.walk(p):
            dirs[:] = [d for d in dirs if d not in ("tools", "build")]
            found += [os.path.join(root, f).replace(os.sep, "/")
                      for f in sorted(files) if f.endswith((".c", ".h"))]
    return found


NUMBER = re.compile(r"\d[\d,._]*\d|\d")
NAMED = re.compile(r"\b\w+\(\)|\b[A-Z][A-Z0-9_]{3,}\b|\b[\w/]+\.[ch]\b")
NEGATION = re.compile(r"\b(not|never|cannot|can't|no longer|without|nothing|"
                      r"neither|fails?|starved?|broke)\b", re.I)

VERDICT = """You are reviewing a shortened source-code comment for lost or \
distorted meaning. Reply with one line only: either `OK` or \
`LOSS: <one sentence>`.

Flag it if the NEW text:
- states a reason that differs from the reason the OLD text gave
- reads as a benefit where the OLD text described a limitation or a bug
- claims something the OLD text did not claim
Do not flag it merely for being shorter or for dropping detail.

OLD:
{old}

NEW:
{new}
"""


def facts_lost(old, new):
    """Concrete tokens present in the original and absent from the rewrite.

    Deterministic, and it catches the failure that matters most: a measured
    number or a named function quietly not making it across. No model needed
    and no model opinion to distrust.
    """
    def missing(pattern, text, other):
        norm = other.replace(",", "")
        return sorted({m for m in pattern.findall(text)
                       if m.replace(",", "") not in norm})

    lost = []
    for n in missing(NUMBER, old, new):
        lost.append(f"number {n}")
    for n in missing(NAMED, old, new):
        lost.append(n)
    if NEGATION.search(old) and not NEGATION.search(new):
        lost.append("every negation dropped (a limitation may now read as a "
                    "benefit)")
    return lost


def review(rows, opts, log):
    """Check each rewrite for dropped facts, then optionally ask a model.

    A deletion is always flagged for a human look - facts_lost trivially
    reports everything as lost against empty text, and a reviewer model has
    nothing to compare a verdict against, so neither is worth spending on it.
    """
    for i, r in enumerate(rows, 1):
        if r.get("deleted"):
            r["lost"] = ["entire comment removed - verify nothing here was "
                         "load-bearing"]
            r["verdict"] = ""
        else:
            r["lost"] = facts_lost(r["original"], r["new"])
            r["verdict"] = ""
            if opts["review_model"] != "none":
                out = ask(opts["review_model"],
                          VERDICT.format(old=r["original"], new=r["new"]),
                          log)
                r["verdict"] = "" if out.upper().startswith("OK") else out[:400]
        flagged = "!" if (r["lost"] or r["verdict"]) else "."
        print(flagged, end="" if i % 60 else f" {i}\n", flush=True)
    print(flush=True)
    return rows


def write_packet(directory, rows, per_file=40):
    """Emit the was/now pairs for review by a model this script cannot call.

    Prose only - no code, no file contents - so a careful reviewer costs a
    fraction of what drafting cost, and can be a better model than the one
    that drafted.
    """
    os.makedirs(directory, exist_ok=True)
    written = []
    for start in range(0, len(rows), per_file):
        chunk = rows[start:start + per_file]
        path = os.path.join(directory, f"pairs-{start // per_file + 1:03d}.md")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("Review each pair. Reply per item with `N OK` or "
                    "`N LOSS: <one sentence>`. Flag a changed reason, a "
                    "limitation turned into a benefit, or a claim the "
                    "original did not make. Do not flag mere brevity.\n\n")
            for n, r in enumerate(chunk, start + 1):
                new = "(deleted entirely)" if r.get("deleted") else r["new"]
                f.write(f"## {n}  {r['path']}:{r['line']}\n\n")
                f.write(f"OLD: {r['original']}\n\nNEW: {new}\n\n")
        written.append(path)
    return written


def write_review_report(path, rows, opts):
    flagged = [r for r in rows if r.get("lost") or r.get("verdict")]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Comment trim - review\n\n")
        f.write(f"- rewrites checked: {len(rows)}\n")
        f.write(f"- flagged: **{len(flagged)}**"
                f" ({100.0 * len(flagged) / len(rows):.0f}%)\n"
                if rows else "- flagged: 0\n")
        f.write(f"- reviewer: `{opts['review_model']}`"
                " (dropped facts are found without a model)\n\n")
        f.write("## Flagged\n\n")
        for r in flagged:
            f.write(f"### {r['path']}:{r['line']}\n\n")
            if r.get("lost"):
                f.write(f"- dropped: {', '.join(r['lost'])}\n")
            if r.get("verdict"):
                f.write(f"- reviewer: {r['verdict']}\n")
            new = "(deleted entirely)" if r.get("deleted") else r["new"]
            f.write(f"\nOLD: {r['original']}\n\nNEW: {new}\n\n")
        f.write(f"## Passed\n\n{len(rows) - len(flagged)} rewrites raised "
                f"nothing.\n")


def write_report(path, results, opts, seconds):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    t, u = results["trimmed"], results["unresolved"]
    deleted = [r for r in t if r.get("deleted")]
    shortened = [r for r in t if not r.get("deleted")]
    over_aim = [r for r in shortened if r.get("over_aim")]
    saved = sum(r["before"] - r["after"] for r in t)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Comment trim (local model)\n\n")
        f.write(f"- model: `{opts['model']}`\n")
        f.write(f"- aim: {opts['limit']} characters,"
                f" ceiling: {opts['ceiling']} characters\n")
        f.write(f"- ran: {time.strftime('%Y-%m-%d %H:%M')}"
                f" ({seconds / 60:.1f} min)\n")
        f.write(f"- shortened: **{len(shortened)}**"
                f" ({len(shortened) - len(over_aim)} to the aim,"
                f" {len(over_aim)} only within the ceiling),"
                f" deleted entirely: **{len(deleted)}**"
                f" (pure change history - git log owns it),"
                f" left alone: {len(u)}\n")
        f.write(f"- prose removed: {saved:,} characters\n")
        if results["rejected"]:
            f.write(f"- **files discarded (code would have moved): "
                    f"{', '.join(results['rejected'])}**\n")
        if deleted:
            f.write("\n## Deleted entirely\n\nEach was judged to be pure "
                    "change-history narration with no constraint that still "
                    "applies. Verify none of these was actually load-bearing "
                    "before trusting this list.\n\n")
            for r in sorted(deleted, key=lambda r: -r["before"]):
                f.write(f"### {r['path']}:{r['line']}  ({r['before']} chars"
                        f" removed)\n\n")
                f.write(f"**was:** {r['original']}\n\n")
        f.write("\n## Review these\n\nEvery rewrite, longest first. Read the "
                "pair: the new line must not invert a limitation into a "
                "benefit or move a reason onto a different cause.\n\n")
        for r in sorted(shortened, key=lambda r: -r["before"]):
            tag = "  *(over the aim, within ceiling)*" if r.get("over_aim") \
                else ""
            f.write(f"### {r['path']}:{r['line']}"
                    f"  ({r['before']} -> {r['after']}){tag}\n\n")
            f.write(f"**was:** {r['original']}\n\n")
            f.write(f"**now:** {r['new']}\n\n")
        if u:
            f.write("## Left at their original length\n\n")
            for r in u:
                f.write(f"- {r['path']}:{r['line']} ({r['before']} chars,"
                        f" best attempt {r['after']})\n")


def main(argv):
    opts = {"limit": 300, "ceiling": 500, "model": DEFAULT_MODEL, "retries": 3,
            "banners": False, "max": 0, "dry_run": False,
            "review_model": DEFAULT_REVIEW_MODEL}
    report = "scripts/results/comment-trim.md"
    pairs = "scripts/results/comment-trim.json"
    review_only, packet = False, ""
    paths = []
    it = iter(argv)
    for arg in it:
        if arg == "--limit":
            opts["limit"] = int(next(it))
        elif arg == "--ceiling":
            opts["ceiling"] = int(next(it))
        elif arg == "--model":
            opts["model"] = next(it)
        elif arg == "--retries":
            opts["retries"] = int(next(it))
        elif arg == "--max":
            opts["max"] = int(next(it))
        elif arg == "--banners":
            opts["banners"] = True
        elif arg == "--dry-run":
            opts["dry_run"] = True
        elif arg == "--report":
            report = next(it)
        elif arg == "--review":
            review_only = True
        elif arg == "--pairs":
            pairs = next(it)
        elif arg == "--review-model":
            opts["review_model"] = next(it)
        elif arg == "--review-packet":
            packet = next(it)
        elif arg in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            paths.append(arg)

    if review_only or packet:
        return run_review(pairs, packet, opts, review_only)

    files = sources_under(paths or DEFAULT_PATHS)
    log = "scripts/results/comment-trim.server.log"
    os.makedirs(os.path.dirname(log), exist_ok=True)
    open(log, "w", encoding="utf-8").close()

    results = {"trimmed": [], "unresolved": [], "rejected": [], "attempted": 0}
    started = time.time()
    for path in files:
        n = trim_file(path, opts, log, results)
        if n:
            print(f"{path}: {n} shortened", flush=True)
        if opts["max"] and results["attempted"] >= opts["max"]:
            break

    write_report(report, results, opts, time.time() - started)
    with open(pairs, "w", encoding="utf-8", newline="\n") as f:
        json.dump(results["trimmed"], f, indent=1)
    print(f"\nshortened {len(results['trimmed'])}, "
          f"left {len(results['unresolved'])}, "
          f"discarded files {len(results['rejected'])}")
    print(f"report: {report}")
    print(f"pairs:  {pairs}   (feed to --review)")
    print(f"transcript: {log}")
    return 0


def run_review(pairs, packet, opts, review_only):
    """--review / --review-packet: judge a finished run's rewrites."""
    if not os.path.exists(pairs):
        print(f"no pairs file at {pairs} - run a trim first, or pass --pairs",
              file=sys.stderr)
        return 1
    rows = json.load(open(pairs, encoding="utf-8"))
    if not rows:
        print("nothing to review")
        return 0

    if packet:
        written = write_packet(packet, rows)
        print(f"{len(rows)} pairs in {len(written)} file"
              f"{'' if len(written) == 1 else 's'} under {packet}/")
        print("Hand these to whichever reviewer you want - they carry prose "
              "only, no code.")
        if not review_only:
            return 0

    log = "scripts/results/comment-trim-review.server.log"
    os.makedirs(os.path.dirname(log), exist_ok=True)
    open(log, "w", encoding="utf-8").close()
    out = "scripts/results/comment-trim-review.md"
    review(rows, opts, log)
    write_review_report(out, rows, opts)
    flagged = sum(1 for r in rows if r.get("lost") or r.get("verdict"))
    print(f"flagged {flagged} of {len(rows)}")
    print(f"review: {out}")
    return 0


if __name__ == "__main__":
    os.chdir(subprocess.run(["git", "rev-parse", "--show-toplevel"],
                            capture_output=True, text=True,
                            check=True).stdout.strip())
    sys.exit(main(sys.argv[1:]))
