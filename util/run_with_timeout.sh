#!/usr/bin/env bash

set -euo pipefail

TIME_LIMIT="${SIM_TIMEOUT:-5m}"
KILL_AFTER="${SIM_TIMEOUT_KILL_AFTER:-30s}"

if [[ $# -eq 0 ]]; then
    printf 'usage: %s <command> [args ...]\n' "$0" >&2
    exit 1
fi

exec timeout --foreground --signal=TERM --kill-after="$KILL_AFTER" \
    "$TIME_LIMIT" "$@"
