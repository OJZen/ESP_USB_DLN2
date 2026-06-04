#!/usr/bin/env python3
import argparse
import re
import subprocess
import sys
import time

import gpiod
import serial
from gpiod.line import Direction, Value


DEFAULT_CHIP = "/dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00"
DEFAULT_LINES = [0, 1, 2, 3, 4, 5, 6, 7]


class C3Probe:
    def __init__(self, port: str, baud: int, timeout: float,
                 open_delay: float) -> None:
        self.serial = serial.Serial(port, baudrate=baud, timeout=timeout)
        self.serial.dtr = False
        self.serial.rts = False
        time.sleep(open_delay)
        self.serial.reset_input_buffer()

    def close(self) -> None:
        self.serial.close()

    def command(self, command: str, timeout: float = 3.0) -> str:
        deadline = time.monotonic() + timeout
        self.serial.write((command + "\n").encode("ascii"))
        self.serial.flush()

        seen = []
        while time.monotonic() < deadline:
            raw = self.serial.readline()
            if not raw:
                continue

            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue

            seen.append(line)
            if line.startswith("OK "):
                return line
            if line.startswith("ERR "):
                raise RuntimeError(f"C3 command failed: {command}: {line}")

        raise TimeoutError(f"C3 command timeout: {command}; seen={seen!r}")

    def sync(self, attempts: int) -> str:
        last_error = None
        for _ in range(attempts):
            try:
                return self.command("PING", timeout=1.5)
            except (RuntimeError, TimeoutError) as exc:
                last_error = exc
                self.serial.reset_input_buffer()
                time.sleep(0.3)

        raise TimeoutError(
            f"C3 probe did not answer PING after {attempts} attempts; "
            f"last error: {last_error}"
        )

    def reset(self) -> None:
        self.command("RESET")

    def read_mask(self) -> int:
        response = self.command("READ")
        match = re.search(r"0x([0-9a-fA-F]+)", response)
        if not match:
            raise RuntimeError(f"unexpected READ response: {response}")
        return int(match.group(1), 16)

    def drive(self, mask: int, value: int) -> None:
        self.command(f"DRIVE 0x{mask:x} 0x{value:x}")

    def pulse(self, mask: int, count: int, high_ms: int, low_ms: int) -> None:
        self.command(f"PULSE 0x{mask:x} {count} {high_ms} {low_ms}",
                     timeout=max(3.0, count * (high_ms + low_ms) / 1000.0 + 2.0))


def parse_lines(raw: str) -> list[int]:
    return [int(item, 0) for item in raw.replace(",", " ").split()]


def values_to_mask(values: list[Value]) -> int:
    mask = 0
    for index, value in enumerate(values):
        if value == Value.ACTIVE:
            mask |= 1 << index
    return mask


def pattern_name(pattern: int, width: int) -> str:
    return f"0x{pattern:0{(width + 3) // 4}x}"


def check_equal(label: str, expected: int, actual: int, mask: int) -> None:
    if (expected & mask) != (actual & mask):
        raise AssertionError(
            f"{label}: expected {pattern_name(expected & mask, mask.bit_length())}, "
            f"got {pattern_name(actual & mask, mask.bit_length())}"
        )


def test_s3_output_to_c3(chip: str, lines: list[int], c3: C3Probe,
                         settle_s: float) -> None:
    channel_count = len(lines)
    channel_mask = (1 << channel_count) - 1
    patterns = [0, channel_mask] + [1 << index for index in range(channel_count)]

    print("TEST s3-output -> c3-input")
    c3.reset()

    settings = gpiod.LineSettings(
        direction=Direction.OUTPUT,
        output_value=Value.INACTIVE,
    )

    with gpiod.request_lines(
        chip,
        consumer="c3_closed_loop_s3_out",
        config={tuple(lines): settings},
    ) as request:
        for pattern in patterns:
            request.set_values({
                line: Value.ACTIVE if pattern & (1 << index) else Value.INACTIVE
                for index, line in enumerate(lines)
            })
            time.sleep(settle_s)
            observed = c3.read_mask()
            check_equal("s3-output -> c3-input", pattern, observed, channel_mask)

    print("PASS s3-output -> c3-input")


def test_c3_output_to_s3(chip: str, lines: list[int], c3: C3Probe,
                         settle_s: float) -> None:
    channel_count = len(lines)
    channel_mask = (1 << channel_count) - 1
    patterns = [0, channel_mask] + [1 << index for index in range(channel_count)]

    print("TEST c3-output -> s3-input")

    settings = gpiod.LineSettings(direction=Direction.INPUT)
    with gpiod.request_lines(
        chip,
        consumer="c3_closed_loop_s3_in",
        config={tuple(lines): settings},
    ) as request:
        for pattern in patterns:
            c3.drive(channel_mask, pattern)
            time.sleep(settle_s)
            observed = values_to_mask(request.get_values(lines))
            check_equal("c3-output -> s3-input", pattern, observed, channel_mask)

    c3.reset()
    print("PASS c3-output -> s3-input")


def test_c3_pulse_to_s3_event(chip: str, line: int, channel: int,
                              c3: C3Probe) -> None:
    print("TEST c3-pulse -> s3-gpiomon-event")

    mask = 1 << channel
    c3.drive(mask, 0)
    time.sleep(0.1)

    proc = subprocess.Popen(
        [
            "gpiomon",
            "-c",
            chip,
            "-e",
            "both",
            "--num-events",
            "2",
            str(line),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        time.sleep(0.3)
        c3.pulse(mask, 1, 100, 100)
        stdout, stderr = proc.communicate(timeout=5)
    except Exception:
        proc.kill()
        proc.communicate()
        raise
    finally:
        c3.reset()

    if proc.returncode != 0:
        raise RuntimeError(f"gpiomon failed: {stderr.strip()}")

    events = [line for line in stdout.splitlines()
              if "rising" in line or "falling" in line]
    if len(events) < 2:
        raise AssertionError(f"expected at least 2 events, got: {stdout!r}")

    print("PASS c3-pulse -> s3-gpiomon-event")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run safe ESP32-C3 to ESP32-S3 DLN-2 GPIO closed-loop tests."
    )
    parser.add_argument("--c3-port", required=True,
                        help="ESP32-C3 console serial port, for example /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--chip", default=DEFAULT_CHIP)
    parser.add_argument("--lines", default=" ".join(str(line) for line in DEFAULT_LINES),
                        help="S3 DLN line offsets wired to C3 channels")
    parser.add_argument("--settle-ms", type=int, default=50)
    parser.add_argument("--open-delay", type=float, default=1.0,
                        help="seconds to wait after opening the C3 serial port")
    parser.add_argument("--sync-attempts", type=int, default=5,
                        help="number of PING attempts used to synchronize with C3")
    parser.add_argument("--skip-events", action="store_true")
    args = parser.parse_args()

    lines = parse_lines(args.lines)
    if not lines:
        raise SystemExit("no S3 lines selected")
    if len(lines) > 8:
        raise SystemExit("the default C3 probe firmware supports at most 8 channels")

    c3 = C3Probe(args.c3_port, args.baud, timeout=0.2,
                 open_delay=args.open_delay)
    try:
        print(c3.sync(args.sync_attempts))
        print(c3.command("MAP"))
        c3.reset()

        test_s3_output_to_c3(args.chip, lines, c3, args.settle_ms / 1000.0)
        test_c3_output_to_s3(args.chip, lines, c3, args.settle_ms / 1000.0)

        if not args.skip_events:
            test_c3_pulse_to_s3_event(args.chip, lines[0], 0, c3)

        c3.reset()
        print("PASS all closed-loop tests")
        return 0
    finally:
        try:
            c3.reset()
        except Exception:
            pass
        c3.close()


if __name__ == "__main__":
    sys.exit(main())
