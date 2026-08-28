#!/usr/bin/env bash
#
# build_flash.sh --diag, without having to remember or type the flag.
#
#   tools/build_flash_diag.sh [COM_PORT] [IDF_EXPORT]
#
# Same arguments as build_flash.sh otherwise - see that script's own header
# for what --diag actually does and why it exists.

set -euo pipefail

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$DIR/build_flash.sh" --diag "$@"
