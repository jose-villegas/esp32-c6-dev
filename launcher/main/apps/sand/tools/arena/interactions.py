"""Which material-to-material INTERACTION is slowest (bd esp32c6-l5z).

A raw ranking of pairings does not answer this. Wood-settled is dear against
everything, so every Wood row lands near the top whether or not the partner
matters at all - that is a main effect, not an interaction.

So the cost of a pairing is decomposed the usual two-way way:

    cost(A,B)  =  grand mean
               +  row effect of A being the settled material
               +  column effect of B being the poured material
               +  INTERACTION, whatever is left

The residual is the part that exists only because these two specific
materials met. A large positive residual is a pairing that costs more than
either material's own habits predict, and that is the thing worth optimising,
because it is real chemistry rather than one expensive material showing up
again.

Row and column effects are computed on the valid pairings only, and a
material needs a minimum number of them before its effect is trusted -
otherwise a material with two surviving pairings defines its own mean and
every residual against it collapses to zero.
"""
import json
import pathlib
import sys

SP = pathlib.Path(__file__).resolve().parent
BUILD = SP / "build"

MIN_PAIRINGS = 5


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else str(BUILD / "round_robin.json")
    recs = [r for r in json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
            if r["valid"]]

    rows = {}
    cols = {}
    for r in recs:
        rows.setdefault(r["settled_mat"], []).append(r["mean"])
        cols.setdefault(r["poured_mat"], []).append(r["mean"])

    grand = sum(r["mean"] for r in recs) / len(recs)
    row_eff = {m: sum(v) / len(v) - grand for m, v in rows.items()}
    col_eff = {m: sum(v) / len(v) - grand for m, v in cols.items()}

    scored = []
    for r in recs:
        a, b = r["settled_mat"], r["poured_mat"]
        if len(rows[a]) < MIN_PAIRINGS or len(cols[b]) < MIN_PAIRINGS:
            continue
        pred = grand + row_eff[a] + col_eff[b]
        scored.append((r["mean"] - pred, r["mean"], pred, a, b, r))

    print("grand mean %.1f us over %d valid pairings\n" % (grand, len(recs)))

    print("=" * 78)
    print("SLOWEST INTERACTIONS (cost beyond what both materials' habits predict)")
    print("=" * 78)
    print("%-10s %-10s %9s %9s %9s  %s"
          % ("settled", "poured", "actual", "expected", "excess", "worst step"))
    scored.sort(reverse=True)
    for exc, act, pred, a, b, r in scored[:15]:
        print("%-10s %-10s %8.1f %8.1f %+9.1f  %8.1f"
              % (a, b, act, pred, exc, r["worst"]))

    print("\n" + "=" * 78)
    print("SLOWEST PAIRINGS, RAW (mostly one expensive material, shown for contrast)")
    print("=" * 78)
    raw = sorted(recs, key=lambda r: -r["mean"])
    for r in raw[:10]:
        print("%-10s %-10s %8.1f us   worst %8.1f"
              % (r["settled_mat"], r["poured_mat"], r["mean"], r["worst"]))

    print("\n" + "=" * 78)
    print("WORST SINGLE STEP (what a frame budget actually feels)")
    print("=" * 78)
    spike = sorted(recs, key=lambda r: -r["worst"])
    for r in spike[:10]:
        ratio = r["worst"] / r["mean"] if r["mean"] else 0.0
        print("%-10s %-10s worst %9.1f us  mean %8.1f  (%.1fx spike)"
              % (r["settled_mat"], r["poured_mat"], r["worst"], r["mean"], ratio))


if __name__ == "__main__":
    main()
