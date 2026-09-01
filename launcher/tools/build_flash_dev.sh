#!/usr/bin/env bash
#
# build_flash.sh --dev, without having to remember or type the flag.
#
#   tools/build_flash_dev.sh [COM_PORT] [IDF_EXPORT]
#
# Same arguments as build_flash.sh otherwise - see that script's own header
# for what --dev actually does and why it exists (and how it differs from
# --diag / build_flash_diag.sh).

set -euo pipefail

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$DIR/build_flash.sh" --dev "$@"
