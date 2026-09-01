#!/usr/bin/env bash
# One-click launcher for the beads-ui dashboard (npm package `beads-ui`,
# CLI binary `bdui`). Starts the server for this workspace if it isn't
# already running (idempotent) and opens it in the default browser.
#
# Usage:
#   scripts/open-beads-ui.sh              # start (if needed) + open at default port 3000
#   scripts/open-beads-ui.sh --port 4000  # any extra args are passed through to `bdui start`

set -euo pipefail

if ! command -v bdui >/dev/null 2>&1; then
    echo "bdui not found on PATH. Install with: npm install -g beads-ui" >&2
    exit 1
fi

# On Windows, beads-ui's server shells out to `bd` via Node's spawn(shell:false),
# which can only exec real Windows executables -- not the POSIX shim `bd`
# (ENOENT/exit 127) and not `bd.cmd` (EINVAL: Node refuses to spawn .cmd/.bat
# directly without a shell). Point it at the real @beads/bd native bd.exe
# instead, unless the caller already set BD_BIN.
case "$(uname -s 2>/dev/null || true)" in
    MINGW* | MSYS* | CYGWIN*)
        if [ -z "${BD_BIN:-}" ]; then
            npm_root="$(npm root -g 2>/dev/null || true)"
            bd_exe="$npm_root/@beads/bd/bin/bd.exe"
            if [ -n "$npm_root" ] && [ -f "$bd_exe" ]; then
                export BD_BIN="$bd_exe"
            fi
        fi
        ;;
esac

bdui start --open "$@"
