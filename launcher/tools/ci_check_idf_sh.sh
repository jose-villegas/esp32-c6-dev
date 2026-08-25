#!/bin/sh
#
# Exercise tools/idf.sh's POSIX path, on a POSIX host.
#
# This exists because that path cannot be tested where it was written.
# idf.sh has two branches - a Windows one that goes through idf_shim.bat to
# delete MSYSTEM, and a plain one that sources export.sh and calls idf.py
# directly. Development happens on Windows, so the Windows branch gets
# exercised constantly and the plain one never does. It was written blind.
#
# The existing build workflows do not cover it either: they invoke
# `idf.py build` through Espressif's CI action, which never touches this
# wrapper at all.
#
# Run from launcher/ inside the esp-idf CI container, where IDF_PATH is set.

set -eu

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
LAUNCHER_DIR="$(cd "$TOOLS_DIR/.." && pwd)"

if [ -z "${IDF_PATH:-}" ]; then
    echo "IDF_PATH is not set - this is meant to run inside the ESP-IDF" >&2
    echo "CI container (see .github/workflows/shell-scripts.yml)." >&2
    exit 1
fi

# shellcheck source=./idf.sh
. "$TOOLS_DIR/idf.sh"
idf_init "$LAUNCHER_DIR" "$IDF_PATH/export.sh" "$TOOLS_DIR"

# The branch under test. On this host MSYSTEM is unset, so idf_needs_shim()
# must say no - assert that rather than assume it, since taking the Windows
# branch here would fail in a way that looks like a missing file.
if idf_needs_shim; then
    echo "FAIL: idf_needs_shim() is true on a POSIX host - MSYSTEM=${MSYSTEM:-} " >&2
    exit 1
fi
echo "ok: idf_needs_shim() correctly false on a POSIX host"

echo "running: idf --version"
version="$(idf --version)"
echo "  -> $version"

case "$version" in
    *ESP-IDF*) echo "ok: idf() reached idf.py and got a version back" ;;
    *) echo "FAIL: unexpected output from idf --version" >&2; exit 1 ;;
esac

# Arguments have to survive intact, semicolons included. That is the exact
# thing that broke every naive Windows approach (see idf_shim.bat), so it is
# worth proving the POSIX branch does not have its own version of the
# problem - a reconfigure fails loudly if SDKCONFIG_DEFAULTS arrives mangled.
echo "running: reconfigure with a semicolon-separated -D argument"
idf -B build.ci \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.diag" \
    -D SDKCONFIG=build.ci/sdkconfig \
    reconfigure >/dev/null
echo "ok: multi-value -D argument survived the call"

echo "tools/idf.sh POSIX path: all checks passed"
