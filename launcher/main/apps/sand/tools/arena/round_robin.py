"""Round-robin every material against every other, then seed brackets from it.

WHY ROUND-ROBIN FIRST. A bracket pairs arbitrarily, so it can eliminate a real
contender in round one against a material that never got onto the board. The
first tournament did exactly that: Stone and Glass "won" the fastest bracket
at 0.3 us because KIND_STATIC cannot be poured, so only 176 of 16,486 cells
ever existed. That measured whether a material can fill a board, not what it
costs. The full matrix makes the degenerate pairings visible instead of
letting them decide a bracket.

VALIDITY. A pairing counts only when BOTH sides actually materialised: the
settled material reached a real share of its target and the poured material
actually spawned. Everything else is reported but excluded from ranking.

SCORE. Raw mean us, and us per 1000 settled cells - the second is what makes
a half-filled board comparable to a full one, and it is the number the
brackets rank on.
"""
import json
import os
import pathlib
import re
import subprocess
import sys

SP = pathlib.Path(__file__).resolve().parent
BUILD = SP / "build"
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
EXE = BUILD / ("arena_probe" + EXE_SUFFIX)
OUT = BUILD / "round_robin.json"

SETTLED_PCT = 40
POURED_PCT = 10
FILL_FLOOR = 0.5
POUR_FLOOR = 0.25

ROW_RE = re.compile(r"a_n=(\d+) a_want=(\d+) b_n=(\d+) "
                    r"settled=(\d+) moving=(\d+) "
                    r"mean_us=([\d.]+) worst_us=([\d.]+)")


def roster():
    r = subprocess.run([str(EXE), "--list"], capture_output=True, text=True)
    return [m for m in r.stdout.split() if m != "?"]


def play(settled, poured):
    r = subprocess.run([str(EXE), settled, str(SETTLED_PCT),
                        poured, str(POURED_PCT)],
                       capture_output=True, text=True, timeout=1800)
    m = ROW_RE.search(r.stdout)
    if m is None:
        return None
    a_n, a_want, b_n, sat, mov, mean, worst = m.groups()
    b_want = int(a_want) * POURED_PCT // SETTLED_PCT
    return {
        "settled_mat": settled, "poured_mat": poured,
        "a_n": int(a_n), "a_want": int(a_want),
        "b_n": int(b_n), "b_want": b_want,
        "settled_cells": int(sat), "moving_cells": int(mov),
        "mean": float(mean), "worst": float(worst),
        "valid": (int(a_n) >= FILL_FLOOR * int(a_want)
                  and int(b_n) >= POUR_FLOOR * b_want),
    }


def per_kcell(rec):
    """Per 1000 cells of the material PRESENT, not per settled cell.

    Dividing by settled cells made the ranking meaningless: a liquid or gas
    settles almost never (water held 119 cells of 15,583), so the divisor
    went toward zero, water scored 2159 us/kcell against sand's 39 while
    being the cheaper material, and five materials scored inf.
    """
    if rec["a_n"] <= 0:
        return float("inf")
    return rec["mean"] * 1000.0 / rec["a_n"]


def main():
    mats = roster()
    print("roster (%d): %s\n" % (len(mats), " ".join(mats)))
    sys.stdout.flush()

    results = []
    total = len(mats) * (len(mats) - 1)
    done = 0
    for a in mats:
        for b in mats:
            if a == b:
                continue
            rec = play(a, b)
            done += 1
            if rec is None:
                print("  [%3d/%3d] %-10s <- %-10s  RUN FAILED" % (done, total, a, b))
                continue
            results.append(rec)
            print("  [%3d/%3d] %-10s <- %-10s  %8.1f us  %7.2f us/kcell  "
                  "sat %6d/%6d pour %5d/%5d %s"
                  % (done, total, a, b, rec["mean"], per_kcell(rec),
                     rec["a_n"], rec["a_want"], rec["b_n"], rec["b_want"],
                     "" if rec["valid"] else "DEGENERATE"))
            sys.stdout.flush()

    OUT.write_text(json.dumps(results, indent=1), encoding="utf-8")
    report(mats, results)


def report(mats, results):
    valid = [r for r in results if r["valid"]]
    print("\n%d of %d pairings valid\n" % (len(valid), len(results)))

    print("=" * 70)
    print("AS THE SETTLED MATERIAL (what it costs to have it sitting there)")
    print("=" * 70)
    rank = []
    for m in mats:
        rows = [r for r in valid if r["settled_mat"] == m]
        if not rows:
            continue
        rank.append((sum(per_kcell(r) for r in rows) / len(rows),
                     sum(r["mean"] for r in rows) / len(rows), m, len(rows)))
    rank.sort(reverse=True)
    for k, raw, m, n in rank:
        print("  %-10s %8.2f us/kcell   %8.1f us raw   (%d pairings)"
              % (m, k, raw, n))

    print("\n" + "=" * 70)
    print("AS THE POURED MATERIAL (what it costs to pour it onto something)")
    print("=" * 70)
    rank2 = []
    for m in mats:
        rows = [r for r in valid if r["poured_mat"] == m]
        if not rows:
            continue
        rank2.append((sum(r["mean"] for r in rows) / len(rows), m, len(rows)))
    rank2.sort(reverse=True)
    for raw, m, n in rank2:
        print("  %-10s %8.1f us raw   (%d pairings)" % (m, raw, n))

    if rank:
        print("\nslowest settled: %s   fastest settled: %s"
              % (rank[0][2], rank[-1][2]))


if __name__ == "__main__":
    main()
