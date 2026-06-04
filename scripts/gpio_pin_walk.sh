#!/usr/bin/env bash
set -euo pipefail

DEFAULT_CHIP="/dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00"

CHIP="${CHIP:-$DEFAULT_CHIP}"
HIGH_SECONDS="${HIGH_SECONDS:-3}"
LOW_SECONDS="${LOW_SECONDS:-3}"
LINES_ENV="${LINES:-}"

if [[ $# -gt 0 ]]; then
    CHIP="$1"
    shift
fi

if [[ $# -gt 0 ]]; then
    LINES=("$@")
elif [[ -n "$LINES_ENV" ]]; then
    read -r -a LINES <<< "$LINES_ENV"
else
    LINES=(0 1 2 3 4 5 6 7)
fi

active_pid=""

cleanup() {
    if [[ -n "$active_pid" ]] && kill -0 "$active_pid" 2>/dev/null; then
        kill "$active_pid" 2>/dev/null || true
        wait "$active_pid" 2>/dev/null || true
    fi
}

trap cleanup EXIT INT TERM

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        exit 1
    fi
}

hold_line() {
    local line="$1"
    local value="$2"
    local seconds="$3"

    printf '%(%H:%M:%S)T line %s -> %s for %ss\n' -1 "$line" "$value" "$seconds"

    gpioset -c "$CHIP" "${line}=${value}" &
    active_pid="$!"

    sleep 0.05
    if ! kill -0 "$active_pid" 2>/dev/null; then
        wait "$active_pid"
    fi

    sleep "$seconds"
    kill "$active_pid" 2>/dev/null || true
    wait "$active_pid" 2>/dev/null || true
    active_pid=""
}

require_cmd gpioset

if [[ ! -e "$CHIP" ]]; then
    echo "GPIO chip not found: $CHIP" >&2
    exit 1
fi

echo "Using chip: $CHIP"
echo "Lines: ${LINES[*]}"
echo "High: ${HIGH_SECONDS}s, low: ${LOW_SECONDS}s"
echo "Press Ctrl-C to stop."

while true; do
    for line in "${LINES[@]}"; do
        hold_line "$line" 1 "$HIGH_SECONDS"
        hold_line "$line" 0 "$LOW_SECONDS"
    done
done
