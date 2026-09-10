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

The standard is WHY-only, not "shorter but keep everything": cut anything
the code already says, all change history (git log owns that), and idiom
re-explanation repeated across constants - keep only a still-true constraint,
a still-rejected alternative, or a short cross-reference. Most comments end
up far under `--limit`, or DELETED entirely - that is the normal, correct
outcome, not a shortfall. Every deletion is flagged in both reports for a
human to confirm nothing load-bearing went with it. (This standard replaced
an earlier "compress into ~300-500 chars while keeping every fact" one after
review found the old approach bloating files with dated tuning journeys and,
once, an outright fabricated number - see the "Hard rules" in PROMPT below
for the fabrication guard this earned.)

`response_problems()` still runs on every answer before it's accepted: over
the ceiling, meta-commentary, a number the original text never stated, or -
the shape a wholesale hallucinated reply takes - no word in common with what
was actually asked about. A bad answer retries rather than getting written
into a live comment. `--review` afterward is still the real check for
meaning.

A comment over `--skip-over` is left untouched rather than attempted at all -
a single automated rewrite of something this large tends to miss whichever
one sentence in it is still load-bearing, however careful the prompt. These
need a human (or an agent working under direct human review) reading the
whole thing and applying the same WHY-only standard by hand, one paragraph
at a time - not a different automated pass, just a slower, closer one.
Skipping them outright here saves the retries that would only be thrown
away, and the report lists them so they're not silently forgotten.

Usage:
  trim_comments_local.py [options] [<path>...]     (default: the sand app)

Options:
  --limit N       character aim, retried against (default 300)
  --ceiling N     hard cap - a result over this is left unresolved, original
                  text kept (default 500)
  --skip-over N   don't even attempt a comment past this length - it needs
                  manual splitting, not compression (default 1500, 0 to
                  disable and attempt everything)
  --model NAME    ollama model (default qwen2.5-coder:32b-instruct-q4_K_M),
                  used for every comment
  --retries N     attempts per comment before giving up (default 3)
  --banners       also rewrite file/section header banners (off: their `====`
                  rules do not survive re-wrapping)
  --max N         stop after N comments, for a smoke test
  --dry-run       write nothing; still produce the report
  --report PATH   markdown report (default scripts/results/comment-trim.md)
  --log PATH      transcript log (default scripts/results/comment-trim
                  {,-review}.server.log, matching --review below) - give
                  each a distinct --report/--pairs/--log (and --review-report
                  for the review pass) when running more than one instance
                  at once against different files, or they will clobber each
                  other's output.

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
  --review-report PATH  where the review's own report goes (default
                  scripts/results/comment-trim-review.md)
"""

import json
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_comment_length import code_only, scan  # noqa: E402

DEFAULT_MODEL = "qwen2.5-coder:32b-instruct-q4_K_M"
DEFAULT_REVIEW_MODEL = "mistral-nemo:latest"
DEFAULT_PATHS = ["launcher/main/apps/sand"]

PROMPT = """You cut source-code comments down to only what the code doesn't \
already say. Most comments should end up much shorter than {limit} \
characters, or deleted outright - that is the normal, correct result, not a \
shortfall. Reply with NOTHING but the cut prose, OR, if nothing in the \
comment survives the rules below, reply with exactly the single word \
DELETE.

Cut, always:
- Anything restating WHAT the code obviously does - trust well-named
  identifiers and the code itself.
- Change history: dates, old values, "raised/lowered/changed from X to Y",
  a bug's own incident report, an investigation that confirmed something was
  never broken. git log owns all of this. Keep only a WHY that is true of
  the CODE AS IT STANDS TODAY, never a WHY for why an OLD value was chosen.
- Re-explanation of an idiom already established elsewhere in this file
  (e.g. what a chance-in-256 roll is) - say it once, not on every constant.
- Arithmetic or measurement whose only role was explaining why an OLD value
  failed, once that value is no longer in the code.

Keep, only if still true and not obvious from the code:
- A real constraint or invariant - especially "do NOT do X here, because Y"
  warnings that would let a bug come back if ignored.
- A rejected alternative that is STILL rejected for a reason that STILL
  holds (not a tuning journey - just the current shape and why).
- A short cross-reference to where fuller logic or history lives (a
  function name, a doc path) - do not restate what's at the destination.
- How a TEST's own setup is arranged - seeding, ordering, why a fixture
  call sits outside a loop rather than inside it - when that arrangement
  is not visible from the code under test, only from the test itself
  (e.g. calling fixture() once before a 16-trial loop, not once per
  trial, because per-trial would re-seed the RNG and replay one identical
  trial sixteen times).

Hard rules, regardless of length:
- A comment that contains a cross-reference - a function name written
  `like_this()`, a file path such as `sand.c`, or a named test - can
  never be answered with DELETE. Keep the citation alone, stripped of
  everything else: `see step_impulses()'s own comment in sand.c` is
  itself a complete, correct answer. Losing it is not a shorter comment,
  it is navigation the code cannot recover on its own.
- Never invent, compute, or round a number, name, or fact that is not
  already stated (or spelled out in words, e.g. "five thousand") in the
  original text below. If you are not sure a number belongs, leave it out
  entirely rather than guess - an omitted number is safe, a wrong one is not.
- Do not re-attribute a reason: if the text says X is done because of A,
  your version must not say it is done because of B.
- If the text describes a LIMITATION or a problem, your version must still
  read as a limitation, not as a benefit.
- If the text names a copyright holder, a licence (Apache-2.0, MIT, ...),
  or says code was copied/adapted from somewhere else, that attribution
  is not "change history" and must survive verbatim in your version,
  regardless of length - reply DELETE instead of dropping it if nothing
  else in the comment is worth keeping.
- British spelling as in the original (colour, behaviour). Plain prose, no
  bullet lists, no headings, no markdown, no code fences, no preamble.

Comment ({length} characters):
{text}
"""

CITED_RETRY = """You answered DELETE, but that comment points at where its \
reasoning actually lives: {cites}. Deleting it throws away the pointer as \
well as the duplication, and nothing in the code can recover it.

Reply with the cross-reference alone - a sentence or so naming what it \
points at, and nothing that restates what is at the destination.

{text}
"""


RETRY = """That was {got} characters, still over {limit}. Rewrite it shorter. \
Reply with nothing but the prose.

{text}
"""


LOG_LOCK = threading.Lock()


def ask(model, prompt, log):
    """Run one prompt through a local Ollama model.

    Safe to call from multiple threads at once - LOG_LOCK keeps one call's
    write from interleaving with another's in the shared transcript file. A
    failed or empty subprocess becomes an empty string here rather than an
    exception; the caller's own retry loop is what decides whether that
    needs another attempt.
    """
    started = time.time()
    with LOG_LOCK, open(log, "a", encoding="utf-8") as f:
        f.write(f"\n===== {time.strftime('%H:%M:%S')} ===== [{model}]\n"
                f"{prompt}\n")
    r = subprocess.run(
        ["ollama", "run", model, "--think=false", "--nowordwrap"],
        input=prompt, capture_output=True, text=True, encoding="utf-8",
        errors="replace",
    )
    out = clean(r.stdout or "")
    with LOG_LOCK, open(log, "a", encoding="utf-8") as f:
        f.write(f"----- {time.time() - started:.1f}s -----\n{out}\n")
    return out


def clean(raw):
    """Strip the wrapper a chat model puts around the thing you asked for."""
    text = re.sub(r"\x1b\[[0-9;]*m", "", raw)  # ANSI colour codes
    text = re.sub(r"^.*Loaded env from.*\n?", "", text, flags=re.M)
    text = re.sub(r"```[a-z]*\n?", "", text)
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


META_REPLY = re.compile(
    r"\byour (?:next )?repl(?:y|ies)\b|\byou should (?:reply|respond)\b|"
    r"^(?:i will|i'll|i am going to|let me) (?:now )?(?:reply|rewrite|"
    r"provide|shorten)\b|\bas an ai\b", re.I)

# A reply that is ONLY an acknowledgement, with nothing else - observed
# verbatim: "Understood." replacing real comment text outright. Anchored
# start-to-end (not .search()) so this never matches a real rewrite that
# merely happens to start with one of these words.
BARE_ACK = re.compile(
    r"^(?:understood|got it|sure|okay|ok|noted|acknowledged)[.!]?\s*"
    r"(?:please (?:provide|send|give).*)?$", re.I)

# Phrases that only exist in THIS script's own PROMPT/RETRY templates -
# their presence in a reply means the model echoed the instruction back
# instead of following it, also observed verbatim ("That was 102
# characters, still over 100. Reword it shorter.").
PROMPT_ECHO = re.compile(
    r"characters,?\s*still over|reply with nothing but|cut prose|"
    r"rewrite it shorter", re.I)


def looks_like_meta_reply(prose):
    """Catches a model responding to the TASK rather than doing it. Three
    shapes have been observed directly, all short enough to pass the length
    check and easy to mistake for real content: talking about what it will
    reply (META_REPLY - "Your next reply should provide..."), a bare
    acknowledgement with nothing else (BARE_ACK - "Understood."), and an
    echo of this script's own prompt wording back at it (PROMPT_ECHO -
    "That was 102 characters, still over 100. Reword it shorter."). Not
    exhaustive, just the shapes actually seen; treated the same as an empty
    reply - forces a retry rather than being accepted."""
    return bool(META_REPLY.search(prose) or BARE_ACK.match(prose.strip())
                or PROMPT_ECHO.search(prose))


# The one word this script actually tells a model to reply with, plus its
# closest synonyms, used as an imperative VERB (followed by a determiner)
# rather than a noun or a lowercase word in ordinary prose. Case-sensitive
# on purpose: this repo's own house style uses ALL-CAPS emphasis phrases
# ("THE ONLY CHECK...", "HALF OF A TWO-PART GUARANTEE") that must not trip
# this, and ordinary prose legitimately uses these same words lowercase
# ("the compiler can delete the walk").
DIRECTIVE_LEAK = re.compile(
    r"\b(?:DELETE|DISCARD|OMIT|REMOVE)\b\s+"
    r"(?:the|this|that|these|those|it|any|all)\b")


def leaks_directive(original, prose):
    """Catches this script's own instruction wording leaking into a live
    rewrite - observed verbatim, mid-prose: "DELETE the measurement
    details." split_delete() only strips a leading or trailing DELETE, and
    looks_like_meta_reply() only catches talk ABOUT replying, so a
    directive sitting in the MIDDLE of an otherwise plausible rewrite
    passed both. A match already present verbatim in the ORIGINAL comment
    is exempted - a rewrite quoting existing text forward has not leaked
    anything.
    """
    m = DIRECTIVE_LEAK.search(prose)
    return bool(m) and m.group(0) not in original


DIGIT = re.compile(r"\d+")
WORD = re.compile(r"[A-Za-z]{4,}")


def fabricates_number(old, new):
    """A digit in `new` that appears nowhere in `old` - the hard rule this
    script's own PROMPT states ("never invent, compute, or round a number")
    but a model does not reliably follow, confirmed directly: "under a
    second" rounded into a literal "~1s", and a "chance/256"-style field
    comment (no digit at all for the probability) rewritten with an
    invented "1/256". Presence-only, not context-aware - it also catches
    "moisture 1" in one clause licensing a fabricated "1 in 256" in
    another, which a naive digit-set check misses - but it does not verify
    a correctly-reused digit is attached to the same fact twice; that part
    still needs a human read of the pair.
    """
    return bool(DIGIT.findall(new)) and \
        not set(DIGIT.findall(new)) <= set(DIGIT.findall(old))


def shares_vocabulary(old, new):
    """False only for a reply with NOT ONE word of four-plus letters in
    common with what was actually asked about - the shape wholesale
    hallucinated content takes (observed verbatim once: "I gnaw nerves,
    whisper agony. You feel me, I devour." for a comment about foam
    dithering). A real rewrite, however aggressively cut, keeps at
    least one identifier or word from its own subject; this is a floor,
    not a meaning check. Skipped (returns True) for an original too short
    to have real vocabulary to lose, so a terse source comment cannot
    trip this on a legitimate paraphrase.
    """
    old_words = {w.lower() for w in WORD.findall(old)}
    if len(old_words) < 3:
        return True
    return bool(old_words & {w.lower() for w in WORD.findall(new)})


ATTRIBUTION = re.compile(
    r"\bcopyright\b|\(c\)\s*\d{4}|\bapache-?2\.?0\b|\bmit licen[cs]e\b|"
    r"\bbsd licen[cs]e\b|\bgpl\b|\bcopied from\b|\badapted from\b", re.I)


def drops_attribution(old, new):
    """A copyright holder, licence name, or "copied/adapted from" note in
    the original that is gone from the rewrite - confirmed happening for
    real (a Waveshare BSP attribution silently dropped by this exact
    pipeline, caught only by a human content review, not by anything
    automated). Not "change history": an attribution notice does not go
    stale and git log does not substitute for it - the rule in PROMPT
    above says to keep it verbatim, but a model has not reliably done
    that, so this is the deterministic backstop.
    """
    return bool(ATTRIBUTION.search(old)) and not ATTRIBUTION.search(new)


# A wrapped identifier continues across comment lines with a trailing `_-`
# and no separating character on the next line - a house convention (see
# e.g. suite_sand_perf.c's own use of it), not something this script
# invented. Comment.text (and this script's own flattened `prose`) already
# joined those lines with a plain space, so what the wrap looks like here
# is the literal substring "word_- next"; rejoining it is what lets a
# correctly-wrapped citation resolve, and lets a genuinely truncated one -
# nothing valid stitched onto the far side of the hyphen - fail to
# resolve, which is the defect this exists to catch.
# The optional `*` absorbs a C comment gutter, so this is correct whether it
# is handed extracted prose or raw source - a silent no-op on the latter
# would drop exactly the citations a wrap was hiding.
WRAP_JOIN = re.compile(r"(\w+_)-\s+\*?\s*(\w+)")


def rejoin_wraps(text):
    return WRAP_JOIN.sub(r"\1\2", text)


# A function call, a test name, or a source file this repo's own comments
# cite by name.
CITATION = re.compile(
    r"\b[A-Za-z_]\w*\(\)|\btest_[A-Za-z0-9_]+\b|\b[\w][\w/]*\.[ch]\b")

_SOURCE_LOCK = threading.Lock()
_source_ids = None
_source_files = None


def _repo_root():
    r = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                       capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else os.getcwd()


def source_index():
    """Every identifier-shaped token, and every .c/.h filename, under
    launcher/ - built once per process and reused for the rest of the run,
    so resolving a citation costs a dict lookup, not a fresh grep, and a
    long retry loop stays cheap. Loose on purpose: presence anywhere in the
    tree, not proof of a definition - this is the mechanical floor
    (a name that exists nowhere is a defect regardless of how good the
    prose reads), not a meaning check.
    """
    global _source_ids, _source_files
    if _source_ids is None:
        with _SOURCE_LOCK:
            if _source_ids is None:
                ids, files = set(), set()
                root = os.path.join(_repo_root(), "launcher")
                for dirpath, dirs, filenames in os.walk(root):
                    dirs[:] = [d for d in dirs
                              if d not in ("build", "build.diag")]
                    for fn in filenames:
                        if not fn.endswith((".c", ".h")):
                            continue
                        files.add(fn)
                        try:
                            with open(os.path.join(dirpath, fn),
                                      encoding="utf-8", errors="replace") as f:
                                ids.update(re.findall(r"[A-Za-z_]\w*",
                                                      f.read()))
                        except OSError:
                            continue
                _source_ids, _source_files = ids, files
    return _source_ids, _source_files


def cited_tokens(text):
    """Every cross-reference a comment carries - the thing a DELETE would
    throw away along with the duplication it was right to cut."""
    return sorted(set(CITATION.findall(rejoin_wraps(text))))


def unresolved_citations(prose):
    """Cross-references a rewrite makes that resolve nowhere under
    launcher/ - checked after rejoin_wraps() so a correctly-wrapped
    identifier is judged on what it actually spells, not on the space this
    script's own flattening left inside it. The observed failure this
    catches: a rewrite that produced `test_a_screen_of_-` with nothing
    valid stitched onto the far side of the hyphen - a citation to a
    function that does not exist, reading as perfectly good prose.
    """
    text = rejoin_wraps(prose)
    ids, files = source_index()
    bad = []
    for m in CITATION.finditer(text):
        token = m.group(0)
        if token.endswith((".c", ".h")):
            if token.rsplit("/", 1)[-1] not in files:
                bad.append(token)
        else:
            name = token[:-2] if token.endswith("()") else token
            if name not in ids:
                bad.append(token)
    return bad


def response_problems(original, prose, ceiling):
    """Automated defects in a candidate rewrite - the concrete failure
    shapes a model has actually produced in this repo, not a meaning check
    (review() and a human still do that). Used in the retry loop to decide
    whether an answer needs another attempt.
    """
    problems = []
    if len(prose) > ceiling:
        problems.append("over ceiling")
    if looks_like_meta_reply(prose):
        problems.append("meta-reply")
    if leaks_directive(original, prose):
        problems.append("directive leak")
    if fabricates_number(original, prose):
        problems.append("fabricated number")
    if drops_attribution(original, prose):
        problems.append("dropped attribution/licence notice")
    if not shares_vocabulary(original, prose):
        problems.append("no shared vocabulary")
    bad_citations = unresolved_citations(prose)
    if bad_citations:
        problems.append(
            f"unresolved cross-reference: {', '.join(bad_citations)}")
    return problems


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


def current_head():
    r = subprocess.run(["git", "rev-parse", "HEAD"],
                        capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


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
    if opts["skip_over"]:
        # Wave 1 found that anything this large is bundling several topics a
        # single ~300-500 char rewrite cannot hold - every one of the 16 over
        # ~2500 chars needed reverting and manually splitting instead. Don't
        # burn retries chasing a compression that review will just undo.
        skipped = [c for c in targets if c.length > opts["skip_over"]]
        for c in skipped:
            results.setdefault("skipped", []).append(
                {"path": path, "line": c.line, "before": c.length})
        targets = [c for c in targets if c.length <= opts["skip_over"]]
    if not opts["banners"]:
        targets = [c for c in targets if not c.is_banner]
    if not targets:
        return 0

    edits = []
    model = opts["model"]
    for com in targets:
        if opts["max"] and results["attempted"] >= opts["max"]:
            break
        results["attempted"] += 1
        original = com.text

        prose = ask(model, PROMPT.format(limit=opts["limit"],
                    length=com.length, text=original), log)
        wants_delete, prose = split_delete(prose)

        # A DELETE used to be taken at its word - the one answer that
        # skipped response_problems() entirely, and the one the model got
        # wrong most often: it deleted comments whose whole surviving value
        # was the cross-reference they carried. Ask once more, naming what
        # the comment points at; a second DELETE leaves it alone rather than
        # dropping a pointer the code cannot recover.
        cites = cited_tokens(original) if wants_delete else []
        if com.own_line and wants_delete and cites:
            prose = ask(model, CITED_RETRY.format(
                cites=", ".join(cites[:4]), text=original), log)
            wants_delete, prose = split_delete(prose)
            if wants_delete or not prose:
                results["kept"].append({
                    "path": path, "line": com.line, "length": com.length,
                    "reason": "deletes a cross-reference"})
                continue

        if com.own_line and wants_delete:
            row = {"path": path, "line": com.line, "before": com.length,
                   "after": 0, "tries": 1, "kept": False, "deleted": True,
                   "original": original, "new": ""}
            results["trimmed"].append(row)
            start, end = whole_line_span(com, source)
            edits.append((start, end, ""))
            continue

        tries = 1
        while prose and tries < opts["retries"] and \
                (len(prose) > opts["limit"] or
                 response_problems(original, prose, opts["ceiling"])):
            prose = ask(model, RETRY.format(got=len(prose),
                        limit=opts["limit"], text=prose), log)
            tries += 1

        if prose and response_problems(original, prose, opts["ceiling"]):
            # Retries exhausted and it's still a bad answer by one of the
            # concrete measures response_problems() checks - discard rather
            # than risk writing this into a live comment.
            prose = ""

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
        # Something else checking out a different branch in this same
        # worktree mid-run has actually happened here once - see esp32c6-90z.
        # A run this long has to notice before it writes over whatever that
        # left behind, not after.
        now = current_head()
        if opts["expected_head"] is not None and now != opts["expected_head"]:
            print(f"\nABORTING: HEAD moved from {opts['expected_head']} to "
                  f"{now} while this run was in progress - something else "
                  f"checked out a different branch in this worktree. "
                  f"Refusing to write {path}.", file=sys.stderr)
            sys.exit(1)
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
    if drops_attribution(old, new):
        lost.append("copyright/licence attribution dropped - restore "
                    "verbatim, this is not change history")
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
                f" deleted entirely: **{len(deleted)}**,"
                f" left alone: {len(u)}\n")
        f.write(f"- prose removed: {saved:,} characters\n")
        skipped = results.get("skipped") or []
        if skipped:
            f.write(f"- **skipped (over {opts['skip_over']} chars, needs "
                    f"manual splitting instead): {len(skipped)}**\n")
            for r in sorted(skipped, key=lambda r: -r["before"]):
                f.write(f"  - {r['path']}:{r['line']} ({r['before']} chars)\n")
        if results["rejected"]:
            f.write(f"- **files discarded (code would have moved): "
                    f"{', '.join(results['rejected'])}**\n")
        if deleted:
            f.write("\n## Deleted entirely\n\nEach was answered DELETE "
                    "outright - the model's reasoning isn't captured, so "
                    "don't assume it was change history; review of past "
                    "runs found most of these were actually a duplicated "
                    "cross-reference or restated content instead. Verify "
                    "none of these was actually load-bearing before "
                    "trusting this list.\n\n")
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
    opts = {"limit": 300, "ceiling": 500, "model": None,
            "retries": 3, "banners": False, "max": 0, "dry_run": False,
            "review_model": DEFAULT_REVIEW_MODEL, "skip_over": 1500}
    report = "scripts/results/comment-trim.md"
    pairs = "scripts/results/comment-trim.json"
    review_report = "scripts/results/comment-trim-review.md"
    log_path = None
    review_only, packet = False, ""
    paths = []
    it = iter(argv)
    for arg in it:
        if arg == "--limit":
            opts["limit"] = int(next(it))
        elif arg == "--ceiling":
            opts["ceiling"] = int(next(it))
        elif arg == "--skip-over":
            opts["skip_over"] = int(next(it))
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
        elif arg == "--log":
            log_path = next(it)
        elif arg == "--review":
            review_only = True
        elif arg == "--pairs":
            pairs = next(it)
        elif arg == "--review-report":
            review_report = next(it)
        elif arg == "--review-model":
            opts["review_model"] = next(it)
        elif arg == "--review-packet":
            packet = next(it)
        elif arg in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            paths.append(arg)

    if opts["model"] is None:
        opts["model"] = DEFAULT_MODEL

    if review_only or packet:
        review_log = log_path or "scripts/results/comment-trim-review.server.log"
        return run_review(pairs, packet, opts, review_only, review_log,
                          review_report)

    files = sources_under(paths or DEFAULT_PATHS)
    log = log_path or "scripts/results/comment-trim.server.log"
    os.makedirs(os.path.dirname(log), exist_ok=True)
    open(log, "w", encoding="utf-8").close()

    opts["expected_head"] = current_head()

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


def run_review(pairs, packet, opts, review_only, log_path, out):
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

    log = log_path
    os.makedirs(os.path.dirname(log), exist_ok=True)
    open(log, "w", encoding="utf-8").close()
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
