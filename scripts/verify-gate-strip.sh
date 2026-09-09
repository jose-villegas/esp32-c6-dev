#!/bin/sh
#
# Prove that removing the pass gates did not change the program.
#
#   scripts/verify-gate-strip.sh <ref-with-gates> <ref-without-gates>
#
# The pass gates compile to nothing when CONFIG_LAUNCHER_SAND_PASS_GATES is
# off, so gated source built with them off and stripped source are supposed to
# be the same program. "Supposed to" is the problem: some gates restructure
# control flow to exist at all - burn_decay turned an `if/else` into a `bool`
# plus a block - so unwrapping one is a rewrite, not a deletion, and reading
# the diff is exactly the wrong way to check a rewrite.
#
# This checks the object code instead. Both refs MUST be built with gates OFF
# and the sand translation units are disassembled; if a single instruction
# differs, the strip changed the program and the script says which function.
#
# "Built with gates off" is verified against the generated sdkconfig, not
# assumed: capture_ref.sh uses each ref's own sdkconfig.defaults.diag, and a
# ref taken during a round has the option turned on in it. Pass a baseline
# with the option off - branch from the gated ref and turn it off there.
#
# WHY NOT cmp ON THE .obj FILES: they carry DWARF, and DWARF carries line
# numbers, so deleting lines changes the bytes without changing the code.
# WHY NOT --only-section=.text: the build uses -ffunction-sections, so there
# is no monolithic .text - the code lives in .text.<function>.
#
# Debug labels (.LVL*, .LBB*, .LBE*) are dropped for the same reason the .obj
# bytes are not compared: they are line-number artefacts, not code. That means
# their DEFINITIONS and the references to them objdump prints inside operand
# comments - deleting a line renumbers every label after it, so a strip that
# changed nothing still shows <.LVL947> against <.LVL946>. The addresses those
# comments annotate are compared; only the label names are dropped.

set -eu

usage() {
    awk 'NR>=2 && /^# WHY NOT/{exit} NR>=2' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

[ $# -eq 2 ] || { [ "${1:-}" = "-h" ] && usage 0; echo "ERROR: need two refs" >&2; usage 2; }

REF_A="$1"
REF_B="$2"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

OBJDUMP=$(command -v riscv32-esp-elf-objdump 2>/dev/null || true)
if [ -z "$OBJDUMP" ]; then
    OBJDUMP=$(ls -d "$HOME"/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-objdump.exe 2>/dev/null | head -1 || true)
fi
[ -n "$OBJDUMP" ] || { echo "ERROR: riscv32-esp-elf-objdump not found" >&2; exit 1; }

# The sand translation units the gates live in. A gate in a file not listed
# here would go unchecked, so this list is the script's real contract.
UNITS="sand.c sand_gas.c sand_reactions.c sand_liquid.c"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

dump_ref() {
    ref="$1"
    out="$2"

    echo "=== building $ref (gates off) ===" >&2
    sh "$REPO_ROOT/scripts/capture_ref.sh" "$ref" --build-only >/dev/null

    # CHECKED, NOT ASSUMED. capture_ref.sh builds each ref with that ref's OWN
    # sdkconfig.defaults.diag, and a ref taken mid-round has the gates turned
    # ON in it - so "both built with gates off" is a claim about the refs, not
    # something this script controls. Comparing a gates-ON build against a
    # stripped one reports a difference for the obvious wrong reason, and the
    # difference looks exactly like a real one: whole functions appearing and
    # disappearing as inlining shifts.
    cfg="$REPO_ROOT/.claude/capture-worktree/launcher/build.diag/config/sdkconfig.h"
    if [ -f "$cfg" ] && grep -q "define CONFIG_LAUNCHER_SAND_PASS_GATES 1" "$cfg"; then
        echo "ERROR: $ref built with the pass gates ON, so it cannot be a" >&2
        echo "strip baseline. Branch from it, turn the option off in" >&2
        echo "launcher/sdkconfig.defaults.diag, commit, and pass that ref." >&2
        exit 2
    fi

    objdir="$REPO_ROOT/.claude/capture-worktree/launcher/build.diag/esp-idf/main/CMakeFiles/__idf_main.dir/apps/sand"
    : > "$out"
    for u in $UNITS; do
        obj="$objdir/$u.obj"
        [ -f "$obj" ] || { echo "ERROR: $obj not built" >&2; exit 1; }
        printf '===== %s =====\n' "$u" >> "$out"
        "$OBJDUMP" -d --no-show-raw-insn "$obj" \
            | grep -vE '^$|file format|^Disassembly' \
            | grep -vE '<\.L(VL|BB|BE)[0-9]+>:' \
            | sed -E 's/^[[:space:]]*[0-9a-f]+:[[:space:]]*//' \
            | sed -E 's/<\.L[A-Z]+[0-9]+([+-]0x[0-9a-f]+)?>//g' >> "$out"
    done
}

dump_ref "$REF_A" "$WORK/a.txt"
dump_ref "$REF_B" "$WORK/b.txt"

if diff -q "$WORK/a.txt" "$WORK/b.txt" >/dev/null; then
    echo
    echo "=== IDENTICAL: $REF_A and $REF_B compile to the same code ==="
    echo "Units checked: $UNITS"
    exit 0
fi

echo
echo "=== DIFFERENT: the strip changed the program ===" >&2
echo "Functions whose disassembly moved:" >&2
diff "$WORK/a.txt" "$WORK/b.txt" | grep -oE '<[a-zA-Z_][a-zA-Z0-9_.]*>:' | sort -u | sed 's/^/  /' >&2
echo >&2
echo "First 40 differing lines:" >&2
diff "$WORK/a.txt" "$WORK/b.txt" | head -40 >&2
exit 1
