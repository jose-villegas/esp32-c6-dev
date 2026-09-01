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

bdui start --open "$@"
