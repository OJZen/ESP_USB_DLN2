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

if [[ ! -e "$CHIP" ]]; then
    echo "GPIO chip not found: $CHIP" >&2
    exit 1
fi

exec python3 - "$CHIP" "$HIGH_SECONDS" "$LOW_SECONDS" "${LINES[@]}" <<'PY'
import re
import signal
import sys
import time

try:
    import gpiod
    from gpiod.line import Direction, Value
except Exception as exc:
    raise SystemExit(f"python-gpiod is required: {exc}")


def parse_period(raw: str) -> float:
    match = re.fullmatch(r"\s*([0-9]+(?:\.[0-9]+)?)([a-zA-Z]*)\s*", raw)
    if not match:
        raise SystemExit(f"invalid period: {raw!r}")

    value = float(match.group(1))
    unit = match.group(2) or "s"

    if unit == "s":
        return value
    if unit == "ms":
        return value / 1000.0
    if unit == "us":
        return value / 1_000_000.0
    if unit == "m":
        return value * 60.0

    raise SystemExit(f"unsupported period unit: {unit!r}")


def timestamp() -> str:
    return time.strftime("%H:%M:%S")


def log(message: str) -> None:
    print(f"{timestamp()} {message}", flush=True)


def stop_handler(signum, frame) -> None:
    raise KeyboardInterrupt


signal.signal(signal.SIGTERM, stop_handler)

chip = sys.argv[1]
high_period = parse_period(sys.argv[2])
low_period = parse_period(sys.argv[3])
lines = [int(arg) for arg in sys.argv[4:]]

if not lines:
    raise SystemExit("no GPIO lines selected")

settings = gpiod.LineSettings(
    direction=Direction.OUTPUT,
    output_value=Value.INACTIVE,
)

high_values = {line: Value.ACTIVE for line in lines}
low_values = {line: Value.INACTIVE for line in lines}

print(f"Using chip: {chip}", flush=True)
print(f"Lines: {' '.join(str(line) for line in lines)}", flush=True)
print(f"High: {sys.argv[2]}, low: {sys.argv[3]}", flush=True)
print("All selected lines are toggled together. Press Ctrl-C to stop.", flush=True)

with gpiod.request_lines(
    chip,
    consumer="gpio_pin_walk",
    config={tuple(lines): settings},
) as request:
    try:
        while True:
            request.set_values(high_values)
            log(f"SET HIGH lines {lines} for {sys.argv[2]}")
            time.sleep(high_period)

            request.set_values(low_values)
            log(f"SET LOW  lines {lines} for {sys.argv[3]}")
            time.sleep(low_period)
    except KeyboardInterrupt:
        request.set_values(low_values)
        log(f"STOP requested; SET LOW lines {lines} before release")
PY
