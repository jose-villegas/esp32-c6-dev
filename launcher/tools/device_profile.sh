# Load one device profile from device_profiles/, for POSIX sh callers.
#
#   . "$LAUNCHER/tools/device_profile.sh"
#   device_profile_load "" "$LAUNCHER/tools/device_profiles"
#   echo "$DP_MAIN_TASK_STACK_BYTES"
#
# An empty first argument means "$DEVICE_PROFILE, else esp32c6".
#
# WHY THIS EXISTS: a second board (S3, P4, C3) may join the test family, and
# the numbers that make the host resemble a device - stack size, free heap,
# icache geometry, the codegen-shaping flags, the QEMU route - are all
# per-chip. They live in exactly one file per chip so that adding a device
# is adding a profile plus its first capture, and so that no gate anywhere
# in the tree carries a chip's number as a literal.
#
# THE FORMAT is plain KEY=value lines and # comments, nothing else - no
# command substitution, no conditionals, no references to other variables.
# That is not stylistic: device_profile.py parses the same files, and the
# moment a profile needs a shell to evaluate it, the Python half either
# reimplements sh or shells out. Keep them dumb.
#
# UNMEASURED FIELDS: a value of the literal string "unmeasured" means "no
# board of this kind has been captured here". device_profile_require reads a
# field and fails loudly on that string, so a gate can never quietly run
# against a number nobody measured.

# Sources the named profile into the current shell.
#
#   device_profile_load [name] [profiles-dir]
#
# Name comes from the argument, else $DEVICE_PROFILE, else esp32c6 - so
# every existing caller keeps today's behaviour without setting anything.
#
# The directory comes from the argument, else $DEVICE_PROFILE_DIR, else the
# tools/ directory this file sits in. That last one is passed by the caller
# rather than discovered, because POSIX sh gives a sourced file no portable
# way to learn its own path ($0 is the caller's). Callers already know where
# they are; find_cc.sh is sourced the same way one line above, so this costs
# nothing extra at any call site.
device_profile_load() {
    dp_name="${1:-${DEVICE_PROFILE:-esp32c6}}"
    dp_dir="${2:-${DEVICE_PROFILE_DIR:-}}"
    if [ -z "$dp_dir" ]; then
        echo "device_profile: pass the profiles directory, or set DEVICE_PROFILE_DIR" >&2
        return 1
    fi
    dp_file="$dp_dir/$dp_name.sh"

    if [ ! -f "$dp_file" ]; then
        echo "device_profile: no profile '$dp_name' in $dp_dir" >&2
        echo "device_profile: available:" >&2
        for f in "$dp_dir"/*.sh; do
            [ -e "$f" ] || continue
            echo "  $(basename "$f" .sh)" >&2
        done
        return 1
    fi

    # shellcheck disable=SC1090
    . "$dp_file"
    DEVICE_PROFILE="$dp_name"
    export DEVICE_PROFILE
}

# Prints one field's value, failing if it is unset or "unmeasured".
# Usage: cap=$(device_profile_require DP_FREE_HEAP_BYTES) || exit 1
device_profile_require() {
    dp_field="$1"
    eval "dp_value=\${$dp_field-}"
    if [ -z "$dp_value" ]; then
        echo "device_profile: ${DEVICE_PROFILE:-?} has no $dp_field" >&2
        return 1
    fi
    if [ "$dp_value" = unmeasured ]; then
        echo "device_profile: ${DEVICE_PROFILE:-?}'s $dp_field is unmeasured -" >&2
        echo "  no board of this kind has been captured here. Capture one and" >&2
        echo "  record the number in device_profiles/${DEVICE_PROFILE:-?}.sh," >&2
        echo "  or run with DEVICE_PROFILE=esp32c6." >&2
        return 1
    fi
    printf '%s\n' "$dp_value"
}
