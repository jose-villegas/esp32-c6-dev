#!/bin/sh
#
# Cross-compile one bare-metal oracle image: the portable sand simulation
# plus suite_sand.c's SAND_HOST_PROBE scenes, for QEMU's generic riscv32
# "virt" machine (bd oracle spike - see this directory's README.md for why
# this route exists and what it still needs to actually run).
#
# Usage:
#   ./build_oracle.sh <scene-name> [output-elf]
#
# Examples:
#   ./build_oracle.sh water
#   ./build_oracle.sh wet_earth out/oracle_wet_earth.elf
#
# Scene names are oracle_main.c's SCENES table - run `grep '"' oracle_main.c`
# for the current list, or read launcher/main/apps/sand/tools/perf_probe/
# probe_main.c's own copy (kept in sync by hand, same as that file already
# does relative to suite_sand.c's SAND_HOST_PROBE wrappers).
#
# CODEGEN FIDELITY, same reasoning as perf_probe/build_probe.sh: every flag
# that changes what GCC emits for portable C comes from the ONE device
# profile, never a literal here - the device's own -march (an ISA choice,
# not codegen shaping) is read here too, unlike build_probe.sh's host build,
# because this build genuinely targets that ISA rather than approximating it
# on x86. Do not hand-copy these flags; read them from the profile so they
# can never drift from launcher/build.diag/compile_commands.json again.
#
# WHAT THIS PROVES AND WHAT IT DOESN'T: a clean link here proves the bare-
# metal bring-up (crt0, linker script, newlib syscall floor) is sound and
# that the ELF is genuinely the device's own instruction set and codegen
# shape - not that it runs, and not any kind of timing. See README.md's
# "counts, never microseconds" warning before anyone is tempted to read a
# number out of a QEMU log and call it microseconds.
#
# POSIX sh, same portability reasoning as launcher/test/run_tests.sh and
# perf_probe/build_probe.sh.

set -eu

HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
LAUNCHER="$(CDPATH= cd -- "$HERE/../.." && pwd)"
MAIN_DIR="$LAUNCHER/main"
TEST_DIR="$LAUNCHER/test"
APP_SAND="$MAIN_DIR/apps/sand"

SCENE="${1:?usage: build_oracle.sh <scene-name> [output-elf]}"
OUT="${2:-$HERE/out/oracle_$SCENE.elf}"

# DEVICE_PROFILE picks the chip (default esp32c6) - see device_profile.sh's
# own header for why the profiles directory is passed explicitly.
. "$LAUNCHER/tools/device_profile.sh"
device_profile_load "" "$LAUNCHER/tools/device_profiles"

ARCH_FLAGS=$(device_profile_require DP_ARCH_FLAGS) || exit 1
CODEGEN_FLAGS=$(device_profile_require DP_CODEGEN_FLAGS) || exit 1
STD_FLAG=$(device_profile_require DP_STD_FLAG) || exit 1
TOOLCHAIN_PREFIX=$(device_profile_require DP_TOOLCHAIN_PREFIX) || exit 1
FREE_HEAP_BYTES=$(device_profile_require DP_FREE_HEAP_BYTES) || exit 1

# Locate the cross compiler. Not on PATH unless `idf.py export` has been run
# in this shell - which most of this repo's tooling avoids needing (see
# tools/find_cc.sh's own reasoning for a host compiler) - so fall back to
# where the ESP-IDF tools installer actually puts it, keyed off the same
# TOOLCHAIN_PREFIX the device profile already names rather than a literal
# version string, so a toolchain upgrade needs no edit here.
find_riscv_gcc() {
    candidate="${TOOLCHAIN_PREFIX}-gcc"
    if command -v "$candidate" >/dev/null 2>&1; then
        command -v "$candidate"
        return 0
    fi
    for base in "${IDF_TOOLS_PATH:-}" "${HOME:-}/.espressif" "${USERPROFILE:-}/.espressif"; do
        [ -n "$base" ] || continue
        for bindir in "$base/tools/$TOOLCHAIN_PREFIX"/*/"$TOOLCHAIN_PREFIX/bin"; do
            [ -x "$bindir/$candidate.exe" ] && { echo "$bindir/$candidate.exe"; return 0; }
            [ -x "$bindir/$candidate" ] && { echo "$bindir/$candidate"; return 0; }
        done
    done
    return 1
}

GCC_BIN=$(find_riscv_gcc) || {
    echo "build_oracle: no ${TOOLCHAIN_PREFIX}-gcc found (checked PATH and \$IDF_TOOLS_PATH/\$HOME/.espressif/tools/$TOOLCHAIN_PREFIX/*/$TOOLCHAIN_PREFIX/bin)" >&2
    exit 1
}
SIZE_BIN=$(printf '%s' "$GCC_BIN" | sed "s/${TOOLCHAIN_PREFIX}-gcc/${TOOLCHAIN_PREFIX}-size/")
OBJDUMP_BIN=$(printf '%s' "$GCC_BIN" | sed "s/${TOOLCHAIN_PREFIX}-gcc/${TOOLCHAIN_PREFIX}-objdump/")

CFLAGS="$STD_FLAG $ARCH_FLAGS $CODEGEN_FLAGS -ffreestanding -Wall -Wextra -Wno-unused-parameter -g"
DEFS="-DDEVICE_BUILD -DSAND_HOST_PROBE -DCONFIG_LAUNCHER_DEVELOPMENT=1 -DORACLE_SCENE=\"$SCENE\""
INCS="-I $MAIN_DIR -I $TEST_DIR -I $TEST_DIR/framework -I $TEST_DIR/stubs"

# Portable sand sources - exactly perf_probe/build_probe.sh's own SOURCES
# list minus its host-only stubs (this directory has its own, freestanding
# ones - see README.md for why esp_timer_host.c couldn't be reused as-is).
# app_*.c is deliberately absent: it is the hardware-facing entry point
# (touch/gfx/app_sand.c), not portable logic - see the repo CLAUDE.md's
# "Naming convention the test runner relies on".
DEVICE_BUILD_SOURCES="
$HERE/oracle_main.c
$APP_SAND/tools/perf_probe/gfx_probe_stub.c
$HERE/esp_timer_oracle_stub.c
$TEST_DIR/suites.c
$TEST_DIR/timing.c
$APP_SAND/suite_sand.c
$APP_SAND/sand.c
$APP_SAND/sand_liquid.c
$APP_SAND/sand_gas.c
$APP_SAND/sand_reactions.c
$APP_SAND/material.c
$APP_SAND/palette.c
$APP_SAND/row_runs.c
$APP_SAND/sand_ui.c
$APP_SAND/tilt.c
"

mkdir -p "$(dirname "$OUT")"
OBJ_DIR="$(dirname "$OUT")/obj_$SCENE"
mkdir -p "$OBJ_DIR"

OBJS=""

# unity.c compiled alone, without -include timing.h - same reason
# build_probe.sh does it (forcing timing.h onto unity.c's own compile trips
# RUN_TEST's "#ifndef" guard from the other side and compiles out the real
# UnityDefaultTestRun body). See timing.h's own header.
"$GCC_BIN" $CFLAGS $INCS -c "$TEST_DIR/framework/unity.c" -o "$OBJ_DIR/unity.o"
OBJS="$OBJS $OBJ_DIR/unity.o"

for src in $DEVICE_BUILD_SOURCES; do
    obj="$OBJ_DIR/$(basename "$src" .c).o"
    # shellcheck disable=SC2086
    "$GCC_BIN" $CFLAGS $DEFS $INCS -include "$TEST_DIR/timing.h" -c "$src" -o "$obj"
    OBJS="$OBJS $obj"
done

# The bare-metal floor itself - no DEVICE_BUILD/SAND_HOST_PROBE defs, no
# -include timing.h, neither means anything to crt0.S or console.c.
"$GCC_BIN" $CFLAGS -c "$HERE/crt0.S" -o "$OBJ_DIR/crt0.o"
OBJS="$OBJS $OBJ_DIR/crt0.o"
"$GCC_BIN" $CFLAGS -c "$HERE/console.c" -o "$OBJ_DIR/console.o"
OBJS="$OBJS $OBJ_DIR/console.o"

# shellcheck disable=SC2086
"$GCC_BIN" $ARCH_FLAGS -nostartfiles -T "$HERE/virt.ld" \
    -Wl,--defsym=HEAP_SIZE="$FREE_HEAP_BYTES" \
    -Wl,--no-warn-rwx-segments \
    $OBJS -lm -o "$OUT" -Wl,-Map="$OUT.map"

echo "built: $OUT"
"$SIZE_BIN" "$OUT"
echo "(objdump -d: $OBJDUMP_BIN -d \"$OUT\")"
