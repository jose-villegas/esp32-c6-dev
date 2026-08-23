#!/usr/bin/env python3
"""
cognitive_complexity.py - an approximation of SonarSource's Cognitive
Complexity metric for C, function by function.

WHY COGNITIVE COMPLEXITY, NOT CYCLOMATIC

Cyclomatic complexity counts paths through a function; cognitive complexity
counts how hard a human has to work to hold the function in their head. The
practical difference is nesting: a chain of ten early returns scores low,
because each one is read and discarded independently, while a single
condition three loops deep scores high, because reading it means holding
everything outside it in mind at the same time. That is much closer to what
"this function is getting hard to follow" actually means, and it is why
Sonar uses it rather than cyclomatic complexity for exactly this kind of
"is it time to split this file" question.

WHAT THIS IS AND IS NOT

Sonar's real implementation works from a full AST. This works from braces
and keywords, which is enough to be USEFUL - it agrees with the real thing
on any reasonably-formatted C, and this project's C is reasonably formatted
- but it is not certified to match Sonar's number exactly, and it does not
try to be. Treat the SCORE as a hotspot finder, not an audit result.

THE RULES THIS APPLIES (SonarSource's B1/B2/B3, as they translate to C)

Flat +1, no nesting bonus:
    a plain `else` (an `else if` counts as one `if`, see below)
    each RUN of the same logical operator in a boolean expression -
        `a && b && c` is +1; `a && b || c` is +2, one for each operator
        that differs from the one before it
    `goto`

`break` and `continue` score nothing: Sonar's own rule is that only a
LABELLED jump counts, and C has no such thing - a bare `break;` always
means "the nearest enclosing loop or switch," never a jump to a distant
line the reader has to go find, which is what the rule is actually about.
`goto` is the C construct that rule is aimed at.

+1, PLUS the current nesting depth, and this also adds one level of
nesting for anything found inside it:
    if / else if, for, while, do, switch, the ternary `?:`

A function that calls itself, directly, adds a flat +1 - recursion is
exactly the kind of thing that is easy to miss and hard to hold in mind.

WHAT IS DELIBERATELY NOT COUNTED

`switch` cases themselves add nothing beyond the `switch` - matching Sonar,
which treats a case ladder as one decision to read, not one per label. A
struct literal's braces are not control flow and are not tracked as
nesting at all - only braces that follow one of the keywords above open a
nesting level.

USAGE

    python cognitive_complexity.py [--threshold N] [--top N] PATH...

    PATH may be a file or a directory (searched recursively for *.c). With
    no PATH, searches the current directory. Prints one line per function
    over the threshold (default 15, Sonar's own default "this needs a
    second look" line), then a per-file total, worst functions first.
"""
import argparse
import re
import sys
from pathlib import Path

# Keywords that open a nesting level (B2 in the doc comment above). `else`
# alone is handled separately, since `else if` must count as one `if`, not
# two.
NESTING_KEYWORDS = {"if", "for", "while", "do", "switch"}

FUNC_SIG_RE = re.compile(
    r"^[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*$"
)

WORD_RE = re.compile(r"[A-Za-z_]\w*")


def strip_comments_and_strings(text):
    """Blank out comments, string and char literals - keeping every newline
    so line numbers in any later message still line up - so keywords inside
    them are never mistaken for code. Not a full tokeniser: it does not
    understand raw strings or trigraphs, neither of which this project's C
    uses."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j == -1 else j
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c == '"' or c == "'":
            quote = c
            j = i + 1
            while j < n and text[j] != quote:
                j += 1 + (1 if text[j] == "\\" and j + 1 < n else 0)
            j = min(j + 1, n)
            out.append("\n" * text.count("\n", i, j))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def find_functions(clean_text):
    """(name, start_of_body_after_open_brace, end_of_body_at_close_brace)
    for every top-level function definition.

    Relies on this project's brace style: the opening `{` of a function
    definition is alone on its own line, right after the line(s) making up
    just the signature. That is a narrow pattern on purpose - narrow enough
    that it does not also match an `if` or a struct literal, which a looser
    "line ending in `{`" rule would.

    The signature itself may span several lines - a wrapped parameter list is
    common in this project's style - so a candidate is accumulated line by
    line rather than matched against a single one, stopping as soon as it
    looks closed (ends in `)`) or clearly is not a signature at all (ends in
    `;` or `{` before ever closing)."""
    lines = clean_text.split("\n")
    functions = []
    offset = 0
    line_offsets = []
    for line in lines:
        line_offsets.append(offset)
        offset += len(line) + 1

    # Only ever scanned OUTSIDE a function body - find_functions never
    # recurses into one, it jumps straight past it - so this does not need
    # its own brace-depth tracking at all.
    i = 0
    while i < len(lines):
        stripped = lines[i].strip()
        if not stripped or stripped.endswith((";", "{", "}")):
            i += 1
            continue

        buf = stripped
        j = i
        while not buf.rstrip().endswith(")") and j + 1 < len(lines):
            j += 1
            nxt = lines[j].strip()
            if not nxt or nxt.endswith((";", "{")):
                break               # not a signature after all
            buf += " " + nxt

        joined = " ".join(buf.split())     # collapse across the join points
        m = FUNC_SIG_RE.match(joined)
        if m and j + 1 < len(lines) and lines[j + 1].strip() == "{":
            name = m.group(1)
            brace_line = j + 1
            start = line_offsets[brace_line] + \
                lines[brace_line].index("{") + 1
            depth = 1
            k = start
            text = clean_text
            while depth > 0 and k < len(text):
                if text[k] == "{":
                    depth += 1
                elif text[k] == "}":
                    depth -= 1
                k += 1
            functions.append((name, start, k - 1))
            i = clean_text.count("\n", 0, k)
            continue
        i += 1
    return functions


def score_function(body, name):
    """Cognitive complexity of one function body - see the module doc
    comment for the rules this applies.

    Which brace belongs to which keyword is the one genuinely fiddly part.
    Looking at "the word right before `{`" does not work - by the time a
    scanner reaches the `{` of `if (x > 0) {`, the last word it saw was `x`,
    not `if`. Instead, a keyword that wants a block ARMS a `pending` slot;
    parentheses are tracked separately so the condition between `if` and its
    `{` cannot be mistaken for the end of the statement; and whatever `{` is
    next, at paren depth zero, is the one that keyword owns. */"""
    score = 0
    nesting = 0
    # What each currently-open brace consumed from `pending` when it opened,
    # so closing it can hand the nesting level back - or None, if it opened
    # as an ordinary block (a struct literal, a bare compound statement) that
    # was never a nesting structure to begin with.
    brace_stack = []
    pending = None
    just_closed_do = False   # so do-while's trailing `while` is not rescored
    paren_depth = 0
    last_logic_op = None

    i, n = 0, len(body)
    while i < n:
        c = body[i]

        if c.isspace():
            i += 1
            continue

        if c == "(":
            paren_depth += 1
            i += 1
            continue
        if c == ")":
            paren_depth = max(0, paren_depth - 1)
            i += 1
            continue

        if c == "{":
            if pending is not None and paren_depth == 0:
                brace_stack.append(pending)
                nesting += 1
                pending = None
            else:
                brace_stack.append(None)
            just_closed_do = False
            last_logic_op = None
            i += 1
            continue

        if c == "}":
            opened_by = brace_stack.pop() if brace_stack else None
            if opened_by is not None:
                nesting -= 1
            just_closed_do = (opened_by == "do")
            last_logic_op = None
            i += 1
            continue

        if c == ";" and paren_depth == 0:
            # A brace-less single statement closes out whatever was pending
            # (`if (x) return;`), and is otherwise just the end of a
            # statement that was never going to open a block at all.
            pending = None
            just_closed_do = False
            last_logic_op = None
            i += 1
            continue

        m = WORD_RE.match(body, i)
        if m:
            word, end = m.group(0), m.end()
            # Reset only when this word starts a genuinely new construct -
            # an ordinary identifier or operand (an operand of the very
            # logical expression being tracked, among other things) must
            # NOT reset it, or a run like `a && b && c` breaks on `b` and
            # scores as two separate operators instead of one run.
            resets_run = True

            if word == "else":
                rest = body[end:]
                stripped = rest.lstrip()
                gap = len(rest) - len(stripped)
                if stripped[:2] == "if" and (len(stripped) == 2 or
                        not (stripped[2].isalnum() or stripped[2] == "_")):
                    score += 1 + nesting     # else-if: one `if`, not two
                    pending = "if"
                    end += gap + 2           # consume "else", gap, "if" together
                else:
                    score += 1               # bare else: flat, no nesting bonus
                    pending = "else"
            elif word == "while" and just_closed_do:
                resets_run = False           # the `while (cond);` tail of do-while
            elif word in NESTING_KEYWORDS:
                score += 1 + nesting
                pending = word
            elif word == "goto":
                score += 1
            elif word == name:
                # crude recursion check: the function's own name, called -
                # good enough given this is a hotspot finder, not a linter
                after = body[end:end + 8].lstrip()
                if after.startswith("("):
                    score += 1
                resets_run = False
            else:
                resets_run = False           # a plain identifier: e.g. an operand

            just_closed_do = False
            if resets_run:
                last_logic_op = None
            i = end
            continue

        if c == "?":
            score += 1 + nesting
            last_logic_op = None
            just_closed_do = False
            i += 1
            continue

        if body[i:i + 2] in ("&&", "||"):
            op = body[i:i + 2]
            if op != last_logic_op:
                score += 1
                last_logic_op = op
            just_closed_do = False
            i += 2
            continue

        # Any other single character - a comparison, an arithmetic operator,
        # a comma, punctuation. Deliberately does NOT reset last_logic_op:
        # `a && b > 0 && c` is still one run, and `b > 0` sitting between
        # the two `&&` must not make the second one look like a new one.
        just_closed_do = False
        i += 1

    return score


def analyse_file(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    clean = strip_comments_and_strings(text)
    results = []
    for name, start, end in find_functions(clean):
        body = clean[start:end]
        line = clean.count("\n", 0, start) + 1
        results.append((name, line, score_function(body, name)))
    return results


def collect_files(paths):
    files = []
    for p in paths:
        p = Path(p)
        if p.is_dir():
            files.extend(sorted(p.rglob("*.c")))
        elif p.is_file():
            files.append(p)
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", default=["."])
    ap.add_argument("--threshold", type=int, default=15,
                    help="only list functions scoring at or above this (default 15)")
    ap.add_argument("--top", type=int, default=0,
                    help="cap the per-function list to the N worst overall (0 = no cap)")
    args = ap.parse_args()

    files = collect_files(args.paths)
    if not files:
        print("no .c files found under: " + " ".join(args.paths), file=sys.stderr)
        return 1

    per_function = []   # (score, file, line, name)
    per_file_total = {}

    for f in files:
        for name, line, score in analyse_file(f):
            per_file_total[f] = per_file_total.get(f, 0) + score
            if score >= args.threshold:
                per_function.append((score, f, line, name))

    per_function.sort(key=lambda t: -t[0])
    if args.top:
        per_function = per_function[:args.top]

    print(f"functions scoring >= {args.threshold}:")
    if not per_function:
        print("  none")
    for score, f, line, name in per_function:
        print(f"  {score:4d}  {f}:{line}  {name}()")

    print()
    print("file totals (sum of every function's score, worst first):")
    for f, total in sorted(per_file_total.items(), key=lambda t: -t[1]):
        print(f"  {total:5d}  {f}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
