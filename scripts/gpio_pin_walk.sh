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

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        exit 1
    fi
}

format_period() {
    local period="$1"

    if [[ "$period" =~ [[:alpha:]] ]]; then
        printf '%s\n' "$period"
    else
        printf '%ss\n' "$period"
    fi
}

require_cmd gpioset

if [[ ! -e "$CHIP" ]]; then
    echo "GPIO chip not found: $CHIP" >&2
    exit 1
fi

echo "Using chip: $CHIP"
echo "Lines: ${LINES[*]}"
high_period="$(format_period "$HIGH_SECONDS")"
low_period="$(format_period "$LOW_SECONDS")"

echo "High: $high_period, low: $low_period"
echo "All selected lines start high, then toggle together forever."
echo "Press Ctrl-C to stop."

args=()
for line in "${LINES[@]}"; do
    args+=("${line}=1")
done

exec gpioset -c "$CHIP" -t "$high_period,$low_period" "${args[@]}"
