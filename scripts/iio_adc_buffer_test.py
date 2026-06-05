#!/usr/bin/env python3
import argparse
import os
import select
import struct
import time
from pathlib import Path


def write_text(path: Path, value: str) -> None:
    path.write_text(str(value), encoding="ascii")


def read_text(path: Path) -> str:
    return path.read_text(encoding="ascii").strip()


def find_iio_device(name: str) -> Path:
    for path in sorted(Path("/sys/bus/iio/devices").glob("iio:device*")):
        name_path = path / "name"
        if name_path.exists() and read_text(name_path) == name:
            return path
    raise SystemExit(f"IIO device named {name!r} not found")


def available_voltage_channels(device: Path) -> list[int]:
    channels = []
    for path in sorted((device / "scan_elements").glob("in_voltage*_en")):
        raw = path.name.removeprefix("in_voltage").removesuffix("_en")
        channels.append(int(raw))
    return channels


def set_buffer_enabled(device: Path, enabled: bool) -> None:
    write_text(device / "buffer" / "enable", "1" if enabled else "0")


def configure_scan(device: Path, channels: list[int]) -> None:
    scan_dir = device / "scan_elements"

    for path in scan_dir.glob("in_voltage*_en"):
        write_text(path, "0")

    timestamp = scan_dir / "in_timestamp_en"
    if timestamp.exists():
        write_text(timestamp, "0")

    for channel in channels:
        write_text(scan_dir / f"in_voltage{channel}_en", "1")


def read_samples(dev_node: Path, channels: list[int], count: int,
                 timeout_s: float) -> list[tuple[int, ...]]:
    sample_size = len(channels) * 2
    target_size = sample_size * count
    deadline = time.monotonic() + timeout_s
    data = bytearray()

    fd = os.open(dev_node, os.O_RDONLY | os.O_NONBLOCK)
    try:
        while len(data) < target_size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break

            readable, _, _ = select.select([fd], [], [], remaining)
            if not readable:
                break

            chunk = os.read(fd, target_size - len(data))
            if not chunk:
                break
            data.extend(chunk)
    finally:
        os.close(fd)

    samples = []
    for offset in range(0, len(data) - sample_size + 1, sample_size):
        values = struct.unpack_from("<" + "H" * len(channels), data, offset)
        samples.append(values)

    return samples


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Read buffered samples from the DLN-2 ADC IIO device."
    )
    parser.add_argument("--device-name", default="dln2-adc")
    parser.add_argument("--channels", default="0",
                        help="space or comma separated channel numbers")
    parser.add_argument("--frequency", default="10",
                        help="sampling_frequency value to write")
    parser.add_argument("--length", default="8",
                        help="IIO buffer length")
    parser.add_argument("--samples", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()

    channels = [
        int(item)
        for item in args.channels.replace(",", " ").split()
        if item
    ]
    if not channels:
        raise SystemExit("no channels selected")

    device = find_iio_device(args.device_name)
    valid_channels = available_voltage_channels(device)
    invalid = [channel for channel in channels if channel not in valid_channels]
    if invalid:
        raise SystemExit(f"invalid channels {invalid}; available={valid_channels}")

    dev_node = Path("/dev") / device.name

    try:
        set_buffer_enabled(device, False)
        configure_scan(device, channels)
        write_text(device / "sampling_frequency", args.frequency)
        write_text(device / "buffer" / "length", args.length)
        set_buffer_enabled(device, True)

        samples = read_samples(dev_node, channels, args.samples, args.timeout)
    finally:
        try:
            set_buffer_enabled(device, False)
        except Exception:
            pass
        try:
            configure_scan(device, [])
        except Exception:
            pass

    print(f"device={device} dev={dev_node}")
    print(f"channels={channels} frequency={args.frequency}Hz samples={len(samples)}")
    for index, sample in enumerate(samples):
        values = " ".join(
            f"in_voltage{channel}={value}"
            for channel, value in zip(channels, sample)
        )
        print(f"{index}: {values}")

    return 0 if len(samples) == args.samples else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PermissionError as exc:
        raise SystemExit(
            f"permission denied: {exc.filename}; run as root or add an IIO udev rule"
        )
