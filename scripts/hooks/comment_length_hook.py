#!/usr/bin/env python3
"""Claude Code PostToolUse hook: refuse a just-written comment over the limit.

Reads the hook payload on stdin and scans ONLY the text the tool wrote - an
Edit's new_string, a Write's content - not the whole file. That is the point:
it must fire on what the model just produced, and stay silent about the
backlog already in the tree, which is somebody's cleanup task and not this
edit's problem.

Exit 2 hands the message back to the model as a blocking error.
"""

import json
import os
import sys

TARGET = 300  # aim for this
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
    """The text this tool call put into the file, or None if not applicable."""
    tool = payload.get("tool_name", "")
    inp = payload.get("tool_input") or {}
    path = inp.get("file_path") or ""
    if not path.endswith((".c", ".h", ".cpp", ".hpp")):
        return None, path
    if tool == "Edit":
        return inp.get("new_string") or "", path
    if tool == "Write":
        return inp.get("content") or "", path
    if tool == "NotebookEdit":
        return None, path
    return None, path


def main():
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0

    text, path = written_text(payload)
    if not text:
        return 0

    # A section header's drawn `====` rule counts toward the length, and asked
    # to fit, a model deletes the rule - observed twice. The rule is aimed at
    # comments beside code, so headers are exempt. `has_rule`, not `is_banner`:
    # the latter also treats line 1 as a header, and line 1 of an edit fragment
    # is wherever the fragment happens to start.
    over = [c for c in scan(path, text)
            if c.length > LIMIT and not c.has_rule]
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
