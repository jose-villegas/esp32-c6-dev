#!/bin/sh
#
# The same stack-frame gate as run_tests.sh's, but against the frames the
# DEVICE's own compiler computes - the ground truth the host pass
# approximates.
#
#   ./check_stack_usage_device.sh
#
# Opt-in, not part of run_tests.sh: it needs the ESP RISC-V toolchain, which
# a contributor running the host suite may not have. The host pass is the
# gate; this is how you check the gate is still telling the truth.
#
# It compiles the app suites for the target with -fstack-usage, using the
# device profile's own ISA and codegen flags, and runs the same
# check_stack_usage.py over the result. No device, no flash, no idf.py -
# suite_sand.c is portable C, so the cross compiler alone is enough to get
# real target frames.
#
# WHY THIS IS WORTH RUNNING: the host gate is only useful if a frame that
# fits on x86 cannot secretly be larger on RISC-V. Measured 2026-09-03 over
# the 411 functions of suite_sand.c present in both builds: not one had a
# larger frame on RISC-V than on x86 (median 0.40x, worst case 0.99x), and
# no function crossed the ceiling on device without also crossing it on the
# host. The host is a conservative over-estimate, which is the safe
# direction for a gate to be wrong in. Re-run this after a toolchain or
# -O-level change, which is the kind of thing that could invert it.
#
# POSIX sh, same portability reasoning as run_tests.sh.

set -eu

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MAIN_DIR=$(CDPATH= cd -- "$TEST_DIR/../main" && pwd)
BUILD_DIR="${TEST_BUILD_DIR:-$TEST_DIR/build}/su-device"

# shellcheck source=../tools/device_profile.sh
. "$TEST_DIR/../tools/device_profile.sh"
device_profile_load "" "$TEST_DIR/../tools/device_profiles" || exit 1

ARCH_FLAGS=$(device_profile_require DP_ARCH_FLAGS) || exit 1
CODEGEN_FLAGS=$(device_profile_require DP_CODEGEN_FLAGS) || exit 1
STD_FLAG=$(device_profile_require DP_STD_FLAG) || exit 1
PREFIX=$(device_profile_require DP_TOOLCHAIN_PREFIX) || exit 1

CC_BIN=$(command -v "$PREFIX-gcc" || true)
if [ -z "$CC_BIN" ]; then
    # Not on PATH under Git Bash unless the IDF environment was activated,
    # which costs ~90s and is not worth paying just to read frame sizes.
    for c in "$HOME/.espressif/tools/$PREFIX"/*/"$PREFIX"/bin/"$PREFIX-gcc.exe" \
             "$HOME/.espressif/tools/$PREFIX"/*/"$PREFIX"/bin/"$PREFIX-gcc"; do
        [ -x "$c" ] && CC_BIN="$c" && break
    done
fi
if [ -z "$CC_BIN" ]; then
    echo "no $PREFIX-gcc found - install the ESP-IDF toolchain, or run the" >&2
    echo "host gate alone via run_tests.sh." >&2
    exit 1
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# -ffreestanding because there is no target libc startup involved here: this
# only ever compiles, never links. DEVICE_BUILD and SAND_HOST_PROBE match
# what the device selftest and the oracle compile the suites with, so the
# frames measured are the ones that actually run on the board.
for f in "$MAIN_DIR"/apps/*/suite_*.c; do
    [ -e "$f" ] || continue
    base=$(basename "$f" .c)
    # shellcheck disable=SC2086
    "$CC_BIN" $STD_FLAG $ARCH_FLAGS $CODEGEN_FLAGS -ffreestanding \
        -Wall -Wextra -Wno-unused-parameter -g \
        -DDEVICE_BUILD -DSAND_HOST_PROBE -DCONFIG_LAUNCHER_DEVELOPMENT=1 \
        -I "$MAIN_DIR" -I "$TEST_DIR" -I "$TEST_DIR/framework" \
        -I "$TEST_DIR/stubs" \
        -I "$TEST_DIR/../components/microui/include" \
        -I "$TEST_DIR/../components/small3dlib/include" \
        -include "$TEST_DIR/timing.h" \
        -fstack-usage -c "$f" -o "$BUILD_DIR/$base.o"
done

if [ -z "$(find "$BUILD_DIR" -maxdepth 1 -name '*.su' -print -quit)" ]; then
    echo "no .su files produced by $CC_BIN - refusing to report a clean pass" >&2
    exit 1
fi

PYTHON=$(command -v python3 || command -v python || true)
if [ -z "${PYTHON:-}" ]; then
    echo "no Python found to run check_stack_usage.py" >&2
    exit 1
fi
echo "frames below are the TARGET's own, from $CC_BIN"
"$PYTHON" "$TEST_DIR/check_stack_usage.py" "$BUILD_DIR"
