#!/bin/sh
#
# Build the material-vs-material arena, host-side.
#
#   ./build_arena.sh            # -> build/arena_probe
#   build/arena_probe --list    # every material the grid can hold
#   build/arena_probe Sand 65 Water 10
#
# Then, for the whole tournament:
#
#   python round_robin.py       # every material against every other
#   python interactions.py      # rank the pairings by interaction excess
#
# Plain host gcc, not the device profile: this instrument answers WHICH
# pairing is expensive and by roughly how much, and a ranking does not need
# device codegen fidelity. Anything that has to be priced goes to
# report_performance.sh on the board - see docs/sand/Perf-Instruments.md on
# what a host number can and cannot say.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
sand="$here/../.."
main="$sand/../.."

mkdir -p "$here/build"

# app_*.c is the hardware-facing entry point and is deliberately absent; the
# rest of the app is portable logic and compiles on a laptop unchanged.
gcc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
    -I "$main" -I "$sand" \
    "$here/arena_probe.c" \
    "$sand/sand.c" \
    "$sand/sand_reactions.c" \
    "$sand/sand_impulse.c" \
    "$sand/sand_plants.c" \
    "$sand/sand_gas.c" \
    "$sand/sand_liquid.c" \
    "$sand/material.c" \
    -o "$here/build/arena_probe"

echo "built $here/build/arena_probe"
