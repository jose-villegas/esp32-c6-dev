#!/usr/bin/env python3
"""Claude Code PostToolUse hook: refuse a just-written comment over the limit.

Reads the hook payload on stdin and scans ONLY the text the tool wrote - an
Edit's new_string, a Write's content - not the whole file. That is the point:
it must fire on what the model just produced, and stay silent about the
backlog already in the tree, which is somebody's cleanup task and not this
edit's problem.

Two rules, and the second is what keeps the first from being a treadmill:

  1. Nothing just written may carry a comment over LIMIT.
  2. An edit that TOUCHES a comment already over TARGET may not hand it back
     longer than it found it.

Rule 1 alone lets everything land at 400-490 and the tree drift upward under
a ceiling it never technically breaks - measured: the count over LIMIT fell
while the count over TARGET rose, and the median offender sat at 489. Rule 2
costs a careless edit nothing (leave the comment alone and it passes) and
makes deliberate growth of an already-long comment the one thing you cannot
do by accident.

Exit 2 hands the message back to the model as a blocking error.
"""

import json
import os
import re
import sys

TARGET = 300  # aim for this
# A header is exempt from the character rule and judged on height
# instead. 30 lines says what a module is and what was rejected; 50 is
# already too long, not a comfortable allowance.
BANNER_TARGET = 30
BANNER_LIMIT = 50

# Phrases that only introduce a story about how the code got here. Kept
# narrow on purpose: the looser set in scripts/find_narrative_comments.py
# is for building a worklist, where a false positive costs a glance. Here
# it costs a refused edit, so only openers with no other use qualify.
# style(9) has three comment shapes and none of them has headings inside.
# A comment needing sections is a document, and the code is not where a
# document goes.
CAPS_HEADING = re.compile(r"^[A-Z][A-Z0-9 ,'()/-]{14,}$")

NARRATIVE = re.compile(
    r"(a first attempt|an earlier version|the first version|"
    r"was considered (?:next|first|and)|used to (?:be|do|have|gate|live|"
    r"call|read|gat)|verified and reverted|tried (?:this|that|it) first|"
    r"we (?:tried|first tried)|before the (?:fix|rewrite)|"
    r"after the rewrite|has since been)", re.I)
LIMIT = 500  # hard ceiling - only a comment that truly needs the room stays here

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
try:
    from check_comment_length import scan
except ImportError:
    # Configured user-wide, this hook fires in projects that do not carry the
    # checker. Staying silent there is what lets one settings entry cover every
    # worktree of this repo without breaking every other repo.
    sys.exit(0)


def written_text(payload):
    """(text written, text it replaced, path). `replaced` is None when there is
    nothing to compare against - a Write hands over a whole file with no record
    of what was there, so rule 2 cannot apply to it."""
    tool = payload.get("tool_name", "")
    inp = payload.get("tool_input") or {}
    path = inp.get("file_path") or ""
    if not path.endswith((".c", ".h", ".cpp", ".hpp")):
        return None, None, path
    if tool == "Edit":
        return inp.get("new_string") or "", inp.get("old_string") or "", path
    if tool == "Write":
        return inp.get("content") or "", None, path
    if tool == "NotebookEdit":
        return None, None, path
    return None, None, path


def over_aim_banner_lines(path, text):
    """Lines of header sitting above BANNER_TARGET. The ratchet's second jaw,
    applied to the one kind of comment the character rule cannot see."""
    if not text:
        return 0
    return sum(c.lines for c in scan(path, text)
               if c.has_rule and c.lines > BANNER_TARGET)


def over_aim_total(path, text):
    """Characters of comment sitting above TARGET, banners excluded. Summed
    rather than compared comment-by-comment because an edit may split one
    comment into two or merge two into one, and the question rule 2 asks is
    about the prose as a whole, not about any one block surviving intact."""
    if not text:
        return 0
    return sum(c.length for c in scan(path, text)
               if c.length > TARGET and not c.has_rule)


def main():
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0

    text, replaced, path = written_text(payload)
    if not text:
        return 0

    # A section header's drawn `====` rule counts toward the length, and asked
    # to fit, a model deletes the rule - observed twice. The rule is aimed at
    # comments beside code, so headers are exempt. `has_rule`, not `is_banner`:
    # the latter also treats line 1 as a header, and line 1 of an edit fragment
    # is wherever the fragment happens to start.
    over = [c for c in scan(path, text)
            if c.length > LIMIT and not c.has_rule]

    heads = []
    for c in scan(path, text):
        for raw in c.raw_lines:
            if CAPS_HEADING.match(re.sub(r"^[/* ]+", "", raw).strip()):
                heads.append(c)
                break
    if heads:
        print(f"Comment rule: {len(heads)} comment"
              f"{'' if len(heads) == 1 else 's'} you just wrote to "
              f"{os.path.basename(path)} use an ALL-CAPS heading.",
              file=sys.stderr)
        print("", file=sys.stderr)
        print("This tree follows OpenBSD style(9): a one-line comment, a "
              "'VERY important' one-liner, or a multi-line comment written as "
              "real sentences filled like a paragraph. None of them has "
              "sections. A comment that needs headings is a document - say the "
              "constraint instead, or move it to docs/.", file=sys.stderr)
        return 2

    story = [c for c in scan(path, text)
             if c.length > TARGET and NARRATIVE.search(c.text)]
    if story:
        print(f"Comment rule: {len(story)} comment"
              f"{'' if len(story) == 1 else 's'} you just wrote to "
              f"{os.path.basename(path)} narrate how the code got here.",
              file=sys.stderr)
        for c in sorted(story, key=lambda c: -c.length)[:3]:
            hit = NARRATIVE.search(c.text)
            print(f"  \"{hit.group(0)}\" in: {c.text[:60]}...", file=sys.stderr)
        print("", file=sys.stderr)
        print("A comment states the constraint that holds now - what it is, why "
              "it exists, how it works, short. git log owns the journey. Keep a "
              "measured number where it is the evidence, and a rejected "
              "alternative only where someone would otherwise retry it, as a "
              "clause.", file=sys.stderr)
        return 2

    tall = [c for c in scan(path, text)
            if c.has_rule and c.lines > BANNER_LIMIT]
    if tall:
        print(f"Header height rule: {len(tall)} header"
              f"{'' if len(tall) == 1 else 's'} you just wrote to "
              f"{os.path.basename(path)} "
              f"{'is' if len(tall) == 1 else 'are'} over {BANNER_LIMIT} lines.",
              file=sys.stderr)
        for c in sorted(tall, key=lambda c: -c.lines):
            print(f"  {c.lines} lines: {c.text[:70]}...", file=sys.stderr)
        print("", file=sys.stderr)
        print(f"A header says what the module IS and what was deliberately "
              f"rejected. {BANNER_TARGET} lines is the aim and {BANNER_LIMIT} "
              "is already too long rather than a comfortable allowance. Prose "
              "that belongs beside the code it describes should move there, "
              "where the character rule applies to it.", file=sys.stderr)
        return 2

    was_banner = over_aim_banner_lines(path, replaced)
    now_banner = over_aim_banner_lines(path, text)
    if was_banner and now_banner > was_banner:
        print(f"Header height ratchet: this edit grows a header in "
              f"{os.path.basename(path)} that was already past the "
              f"{BANNER_TARGET}-line aim - {was_banner} lines went in, "
              f"{now_banner} came back.", file=sys.stderr)
        print("", file=sys.stderr)
        print("Leaving it alone is fine; shortening it is better. A header "
              "that keeps growing is where prose goes to escape the character "
              "rule.", file=sys.stderr)
        return 2

    # Rule 2. Only bites when the edit found an over-aim comment there
    # already: a brand-new comment between TARGET and LIMIT is allowed, since
    # TARGET is an aim and LIMIT is the rule.
    was = over_aim_total(path, replaced)
    now = over_aim_total(path, text)
    if not over and was and now > was:
        print(f"Comment length ratchet: this edit lengthens a comment in "
              f"{os.path.basename(path)} that was already past the "
              f"{TARGET}-character aim - {was} characters of over-aim prose "
              f"went in, {now} came back.", file=sys.stderr)
        print("", file=sys.stderr)
        print("Leaving it alone is fine. Making it shorter is better. Making "
              "it longer is the one thing that is not available, because that "
              "is how a tree ends up with hundreds of comments parked just "
              "under the ceiling. Cut whatever the code already says, and any "
              "history git log owns.", file=sys.stderr)
        return 2

    if not over:
        return 0

    name = os.path.basename(path)
    print(f"Comment length rule: {len(over)} comment"
          f"{'' if len(over) == 1 else 's'} you just wrote to {name} "
          f"exceed{'s' if len(over) == 1 else ''} {LIMIT} characters.",
          file=sys.stderr)
    for c in sorted(over, key=lambda c: -c.length):
        print(f"  {c.length} chars: {c.text[:70]}...", file=sys.stderr)
    print("", file=sys.stderr)
    print(f"Rewrite them. {TARGET} characters is the aim; up to {LIMIT} is "
          "fine for a comment that truly needs the room, but this is past "
          "even that. Keep only a WHY the code doesn't already say - a real "
          "constraint, a device measurement, a rejected alternative still "
          "rejected for a reason that still holds. Cut change history (git "
          "log owns dates and old values), anything restating WHAT the code "
          "does, and re-explaining an idiom already established elsewhere. "
          "Deleting the comment entirely is a normal outcome, not a "
          "shortfall.", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
